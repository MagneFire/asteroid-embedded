/* Mobvoi TicWatch Pro (catfish): APQ8009W with a PM8916. */
#include "asteroid.h"

static const char *const firmware_dirs[] = { "/firmware/", NULL };

const struct board board_catfish = {
	.name = "catfish",
	.compatible = "mobvoi,catfish",
	.heart_rate = true,
	/* RM67162 or RM69080, 400x400 command mode on one lane, TE into
	 * the MDP. */
	.panel = {
		.width = 400, .height = 400,
		.hz = 60, .te_gpio = 24, .round = true, .reset_gpio = 25,
	},
	.firmware_dirs = firmware_dirs,
	.firmware_part = "vendor",
	/* BCM43436 on BLSP1 UART2 (gpio 20, 21 function 3; 111, 112
	 * function 2). Both wake lines are active low. Its I/O supply is
	 * L12, which is also the WiFi's. Its ROM takes the 26 MHz crystal for
	 * a 37.4 MHz one, so its UART runs at 0.695 times the rate asked. */
	.bt = {
		.uart = 2, .gpio = 20, .func = 3, .dev_wake = 32, .host_wake = 31, .reg_on = 34, .mpm = 19,
		.dev_wake_active = false, .host_wake_active = false, .ldo = 12, .ldo_uv = 1800000,
		.rom_baud = 80128, .rom_fast = 2083333, .addr_part = "persist", .addr_path = "bt_nv.bin",
		.patch = "BCM43430B0.hcd",
		.sleep_mode = { 1, 0x18, 0x18, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
	},
	.pmic_mpm = 62,
	.key2 = { .gpio = 98, .mpm = 38 },
	.usb_phy_init = { 0x73 },
	/* SMB231 on BLSP1 QUP1, its STAT line on gpio 95. */
	.battery_driver = &battery_smb231,
	.charger = { .i2c = { .bus = 1, .sda = 6, .scl = 7, .func = 3, .addr = 0x12 }, .irq = 95, .mpm = 35, .suspend = 58 },
	.touch = &touch_fts,
	.touch_pins = { .i2c = { .bus = 6, .sda = 10, .scl = 11, .func = 2, .addr = 0x38 }, .irq = 65, .reset = 23, .mpm = 3,
			.ldo = 17, .ldo_uv = 2850000, .ldo_io = 6, .ldo_io_uv = 1800000, .power = 93 },
	/* The STM32 sensor hub on BLSP1 QUP4 (gpio 12, 13, 15, function 1). */
	.hub = &hub_nanohub,
	.hub_pins = { .irq = 97, .spi = 4, .gpio = 12, .func = 1, .cs = 14, .wake_mcu = 71, .reset = 72, .boot0 = 70,
		      .mode_irq = 90, .strobe = 73, .mode = { 8, 9, 99, 110 }, .mpm = 37 },
	/* DRV2605 on BLSP1 QUP6, no trigger line. */
	.motor = &motor_drv2605,
	.motor_pins = { .i2c = { .bus = 6, .sda = 10, .scl = 11, .func = 2, .addr = 0x5a }, .enable = 26,
			.rated = 0x4b, .clamp = 0x54 },
};
