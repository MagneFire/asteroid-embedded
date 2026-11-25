/* LG G Watch R (lenok), the retail rev-10 board. */
#include "asteroid.h"
#include "display/display.h"

/* On BLSP1 QUP2, where the ADSP ran them: the MPU-6515 comes out of
 * reset running, so it is put to sleep. */
static const struct board_chip_off chips_off[] = {
	{ 0x68, 0x6b, 0x00, 0x40, 0, 0xff },
	{ 0 },
};

/* The Bluetooth patch and the GPU microcode are under vendor. */
static const char *const firmware_dirs[] = { "/vendor/firmware/", NULL };

const struct board board_lenok = {
	.name = "lenok",
	.compatible = "lge,lenok",
	.smem_platform = 132,
	.chips_off = { { .bus = 2, .sda = 6, .scl = 7, .func = 3 }, chips_off },
	.heart_rate = false,
	/* LG4237 320x320 command-mode OLED, set up by the bootloader: 328
	 * lines with the porches at 60 Hz, TE on gpio 24. */
	.panel = {
		.width = 320, .height = 320,
		.lines = 328, .hz = 60, .te_gpio = 24, .flip_ud = true, .round = true,
	},
	.firmware_dirs = firmware_dirs,
	/* BCM4343A0 on BLSP1 UART4 as on dory, without its reset line. */
	.bt = {
		.uart = 4, .gpio = 12, .func = 2, .dev_wake = 47, .host_wake = 48, .reg_on = 45, .mpm = 20,
		.dev_wake_active = false, .host_wake_active = false,
		.addr_part = "persist", .addr_path = "bdaddr.txt",
		.patch = "4343A0.hcd",
		.sleep_mode = { 1, 0x18, 0x18, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
	},
	.pmic_mpm = 62,
	/* The bq27421 gauge has no driver yet. */
	.battery_driver = &battery_pmic_cable,
	.usb_phy_init = { 0x44, 0x68, 0x24, 0x13 },
	/* Synaptics RMI4 on BLSP1 QUP5 as on dory, fed by L15 and L22. */
	.touch = &touch_rmi4,
	.touch_pins = { .i2c = { .bus = 5, .sda = 18, .scl = 19, .func = 3, .addr = 0x20 }, .irq = 17, .reset = 16, .mpm = 8,
			.ldo = 15, .ldo_uv = 3300000, .ldo_io = 22, .ldo_io_uv = 1800000, .palm_width = 13 },
};
