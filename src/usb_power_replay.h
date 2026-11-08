#ifndef USB_POWER_REPLAY_H
#define USB_POWER_REPLAY_H

/* Replay a boot-time VBUS level through the nRF UDC's registered callback. */
void usb_power_replay_detected(void);
void usb_power_replay_ready(void);

#endif
