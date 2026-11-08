/*
 * Button gesture detection (interrupt-driven, low power).
 *
 * The button is wired between two adjacent GPIOs:
 *   - sense pin (input, pull-up): reads LOW when pressed (shorted to vgnd)
 *   - vgnd pin (output, LOW): the "virtual ground"
 *
 * Gestures:
 *   - press:   stable low edge (calls button_pressed, used to wake the radio)
 *   - click:   short press. N clicks within CLICK_WINDOW_MS -> button_clicked(N)
 *   - long:    held >= LONG_PRESS_MS -> button_long_pressed()
 *
 * The sense pin generates an interrupt on both edges. On an edge we disable
 * the interrupt, wait DEBOUNCE_MS, sample once, commit the change, then re-arm.
 * Between presses the CPU stays asleep; no periodic polling.
 *
 * A compile-time test poll (BUTTON_TEST_POLL) samples a SWD-settable flag
 * (dbg_force_press) so clicks can be injected via pyocd without a physical
 * button. Disable it for production.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#include "button.h"

#define DEBOUNCE_MS             20
#define DEFAULT_CLICK_WINDOW_MS 500
#define DEFAULT_LONG_PRESS_MS   3000

#define BTN_NODE   DT_NODELABEL(btn0)
#define VGND_NODE  DT_NODELABEL(btn_vgnd)

BUILD_ASSERT(DT_PROP_OR(DT_GPIO_CTLR(BTN_NODE, gpios), sense_edge_mask, 0) &
	     BIT(DT_GPIO_PIN(BTN_NODE, gpios)),
	     "Button input must use GPIO SENSE for low-power edge detection");

static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(BTN_NODE, gpios);
static const struct gpio_dt_spec vgnd = GPIO_DT_SPEC_GET(VGND_NODE, gpios);

static struct gpio_callback btn_cb;
static struct k_work_delayable debounce_work;
static struct k_work_delayable long_press_work;
static struct k_work_delayable click_window_work;
#if IS_ENABLED(CONFIG_TOTP_TEST_POLL)
static struct k_work_delayable test_poll_work;
#endif
static bool pressed;
static bool long_press_reported;
static int click_count;
static bool pairing;
static uint32_t click_window_ms = DEFAULT_CLICK_WINDOW_MS;
static uint32_t long_press_ms = DEFAULT_LONG_PRESS_MS;

/* Debug counters (inspected via SWD). */
volatile int dbg_press;
volatile int dbg_release;
volatile int dbg_longpress;
volatile int dbg_clickwindow;

/* SWD-triggerable button emulation: non-zero forces "pressed". */
volatile int dbg_force_press;

static void click_window_handler(struct k_work *work)
{
	int count = click_count;

	click_count = 0;
	dbg_clickwindow++;
	button_clicked(count);
}

static void long_press_handler(struct k_work *work)
{
	if (pressed) {
		click_count = 0;
		k_work_cancel_delayable(&click_window_work);
		long_press_reported = true;
		dbg_longpress++;
		button_long_pressed();
	}
}

static void process_press(void)
{
	dbg_press++;
	long_press_reported = false;
	/* Do not finalize earlier short clicks while this press may still become
	 * a long press. A short release below reschedules the window. */
	k_work_cancel_delayable(&click_window_work);
	k_work_reschedule(&long_press_work, K_MSEC(long_press_ms));
	button_pressed();
}

static void process_release(void)
{
	dbg_release++;
	k_work_cancel_delayable(&long_press_work);

	if (long_press_reported) {
		return;
	}

	if (pairing) {
		/* Immediate cancel, no click-window wait. */
		button_clicked(1);
	} else {
		click_count++;
		k_work_reschedule(&click_window_work, K_MSEC(click_window_ms));
	}
}

/* Commit a stable state change (used by both the ISR and test-poll paths). */
static void commit_change(bool now_pressed)
{
	if (now_pressed == pressed) {
		return;
	}
	pressed = now_pressed;

	if (now_pressed) {
		process_press();
	} else {
		process_release();
	}
}

static void debounce_handler(struct k_work *work)
{
	bool now_pressed = (gpio_pin_get_raw(btn.port, btn.pin) == 0);

	commit_change(now_pressed);

	/* Re-arm for the next edge. */
	gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_BOTH);
}

static void btn_isr(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	/* Disable the interrupt until the state change is committed. */
	gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_DISABLE);
	k_work_reschedule(&debounce_work, K_MSEC(DEBOUNCE_MS));
}

#if IS_ENABLED(CONFIG_TOTP_TEST_POLL)
static void test_poll_handler(struct k_work *work)
{
	bool now_pressed = (gpio_pin_get_raw(btn.port, btn.pin) == 0);

	if (dbg_force_press) {
		now_pressed = true;
	}
	commit_change(now_pressed);

	k_work_reschedule(&test_poll_work, K_MSEC(DEBOUNCE_MS));
}
#endif

void button_set_pairing(bool p)
{
	pairing = p;
	if (p) {
		/* A long press may begin after one or more short releases. Those
		 * clicks belong to the abandoned normal-mode gesture and must not
		 * later cancel the newly entered pairing mode. */
		click_count = 0;
		k_work_cancel_delayable(&click_window_work);
	}
}

void button_set_timings(uint32_t click_window, uint32_t long_press)
{
	if (click_window > 0) {
		click_window_ms = click_window;
	}
	if (long_press > 0) {
		long_press_ms = long_press;
	}
}

void button_init(void)
{
	if (!gpio_is_ready_dt(&vgnd)) {
		return;
	}
	gpio_pin_configure_dt(&vgnd, GPIO_OUTPUT_LOW);

	if (!gpio_is_ready_dt(&btn)) {
		return;
	}
	gpio_pin_configure_dt(&btn, GPIO_INPUT);

	k_work_init_delayable(&debounce_work, debounce_handler);
	k_work_init_delayable(&long_press_work, long_press_handler);
	k_work_init_delayable(&click_window_work, click_window_handler);

	gpio_init_callback(&btn_cb, btn_isr, BIT(btn.pin));
	gpio_add_callback(btn.port, &btn_cb);
	gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_BOTH);

#if IS_ENABLED(CONFIG_TOTP_TEST_POLL)
	k_work_init_delayable(&test_poll_work, test_poll_handler);
	k_work_reschedule(&test_poll_work, K_MSEC(DEBOUNCE_MS));
#endif
}
