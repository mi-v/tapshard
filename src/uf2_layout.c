/* The UF2 variant must not reuse the SWD image's NVS at 0xf8000. */
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#define CHECK_PARTITION(node, address, length) \
	BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(node)) == (address)); \
	BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(node)) == (length))

CHECK_PARTITION(code_partition, 0x26000, 0xba000);
CHECK_PARTITION(factory_backup_partition, 0xe0000, 0xc000);
CHECK_PARTITION(storage_partition, 0xec000, 0x8000);
CHECK_PARTITION(boot_partition, 0xf4000, 0xa000);
CHECK_PARTITION(mbr_params_partition, 0xfe000, 0x1000);
CHECK_PARTITION(boot_settings_partition, 0xff000, 0x1000);
BUILD_ASSERT(CONFIG_FLASH_LOAD_OFFSET == 0x26000);
BUILD_ASSERT(DT_REG_ADDR(DT_CHOSEN(zephyr_sram)) == 0x20000000);
BUILD_ASSERT(DT_REG_SIZE(DT_CHOSEN(zephyr_sram)) == 0x3fc00);
BUILD_ASSERT(CONFIG_SRAM_SIZE * 1024 == 0x3fc00);
BUILD_ASSERT(CONFIG_SRAM_BASE_ADDRESS == 0x20000000);
