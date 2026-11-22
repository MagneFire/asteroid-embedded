/* LG G Watch (dory), the retail rev-10 and rev-11 boards. The only key
 * is the power key, read through the PMIC's PON block; it sits under the
 * watch, so nothing here depends on pressing it. The PM8226 charges the
 * cell but its battery monitor is unused: a MAX17050 measures it. */
#include "asteroid.h"
#include "display/display.h"
#include "soc/max17050.h"

/* NT35310: idle lowers the frame rate, as the vendor's ambient mode does;
 * the panel has no idle colour mode. */
static const struct dcs_step idle_on[] = { { 0xb2, 3, 0, { 0x8d, 0xa5, 0xa5 } } };
static const struct dcs_step idle_off[] = { { 0xb2, 3, 0, { 0x8d, 0x8d, 0x8d } } };

/* The vendor's cell model for the 400 mAh cell, as its tree gives it. The
 * gauge takes its temperature from the host, so its thermistor input is
 * off and the status's battery-absent bit means nothing. */
static const struct max17050_model cell_model = {
	.config = 0x2100, .learn_cfg = 0x2606, .filter_cfg = 0x87a4, .relax_cfg = 0x506b, .full_soc_thresh = 0x5f00,
	.design_cap = 0x0320, .fullcap = 0x0320, .fullcapnom = 0x0320, .ichgt_term = 0x00c0,
	.rcomp0 = 0x004f, .tempco = 0x2f35, .qrtbl = { 0x0d84, 0x0a08, 0x0884, 0x0584 },
	.tgain = 0xe932, .toff = 0x2381, .version = 0x0002, .misc_cfg = 0x0870, .v_empty = 0x964c,
	.cell = {
		0xa050, 0xaac0, 0xad80, 0xb410, 0xb500, 0xb7b0, 0xb820, 0xb890, 0xbc30, 0xbc80, 0xc340, 0xc390,
		0xcb40, 0xcc80, 0xd6d0, 0xd790, 0x00c0, 0x0170, 0x01d0, 0x0440, 0x08e0, 0x2db0, 0x0920, 0x0c20,
		0x0cc0, 0x1120, 0x0cc0, 0x0850, 0x0660, 0x0690, 0x0aa0, 0x0aa0, 0x0100, 0x0100, 0x0100, 0x0100,
		0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
	},
};

/* The vendor's battery thermistor table, every 5 C of its 1 C steps. */
static const struct board_therm therm[] = {
	{ -300, 1616 }, { -200, 1491 }, { -100, 1327 }, { 0, 1140 }, { 50, 1045 }, { 100, 954 }, { 150, 869 },
	{ 200, 792 }, { 250, 724 }, { 300, 665 }, { 350, 614 }, { 400, 570 }, { 450, 534 }, { 500, 503 },
	{ 550, 478 }, { 600, 457 }, { 650, 440 }, { 700, 425 }, { 750, 413 }, { 790, 405 },
};

/* The Bluetooth patch and the GPU microcode are under vendor. */
static const char *const firmware_dirs[] = { "/vendor/firmware/", NULL };

/* 400 mAh: 4.35 V, 1 C, 30 mA termination, as the vendor sets it. */
BOARD_CHG_CHECK(4350, 4350, 4450, 400, 400);

const struct board board_dory = {
	.name = "dory",
	.compatible = "lge,dory",
	.heart_rate = false,
	/* NT35310 280x280 command-mode LCD, a window of its 320x480 driver
	 * set up by the bootloader: 327 lines with the porches at 60 Hz, TE
	 * on gpio 24. */
	.panel = {
		.width = 280, .height = 280,
		.lines = 327, .hz = 60, .te_gpio = 24, .flip_ud = false, .round = false, .lcd = true,
		.idle_on = idle_on, .idle_on_n = sizeof idle_on / sizeof idle_on[0],
		.idle_off = idle_off, .idle_off_n = sizeof idle_off / sizeof idle_off[0],
	},
	.firmware_dirs = firmware_dirs,
	/* BCM20715A1 on BLSP1 UART4 (gpio 12-15, function 2) as on sturgeon,
	 * REG_ON on gpio 45, reset on gpio 20, host wake on gpio 48 (MPM pin
	 * 20), device wake on gpio 47. Both wake lines are active low, so the
	 * sleep mode is sturgeon's. The factory address is not known; set it
	 * on the console. */
	.bt = {
		.uart = 4, .gpio = 12, .func = 2, .dev_wake = 47, .host_wake = 48, .reg_on = 45, .reset = 20, .mpm = 20,
		.dev_wake_active = false, .host_wake_active = false,
		.patch = "BCM20702A.hcd",
		.sleep_mode = { 1, 0x18, 0x18, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
	},
	.pmic_mpm = 62,
	.chg = { .vdd_max_mv = 4350, .vdd_safe_mv = 4350, .vin_min_mv = 4450,
		 .ibat_max_ma = 400, .ibat_safe_ma = 400, .iterm_ma = 30, .no_thermistor = true },
	.rsense_uohm = 10000,
	.usb_phy_init = { 0x44, 0x68, 0x24, 0x13 },
	/* MAX17050 on BLSP1 QUP1. */
	.battery_driver = &battery_pmic_max17050,
	.therm = therm,
	.therm_n = sizeof therm / sizeof therm[0],
	.gauge = { .i2c = { .bus = 1, .sda = 2, .scl = 3, .func = 3, .addr = 0x36 }, .model = &cell_model },
	/* Synaptics RMI4 on BLSP1 QUP5 as on sturgeon, fed by L28. */
	.touch = &touch_rmi4,
	.touch_pins = { .i2c = { .bus = 5, .sda = 18, .scl = 19, .func = 3, .addr = 0x20 }, .irq = 17, .reset = 16, .mpm = 8,
			.ldo = 28, .ldo_uv = 3000000, .palm_width = 13 },
	/* No hub: an LIS3DSH on BLSP1 QUP2 for wrist tilt, interrupt 1 on gpio
	 * 33 (MPM pin 13), fed by L15. */
	.hub = &hub_lis3dsh,
	.hub_pins = { .i2c = { .bus = 2, .sda = 6, .scl = 7, .func = 3, .addr = 0x1e }, .irq = 33, .mpm = 13,
		      .ldo = 15, .ldo_uv = 2800000 },
	/* LM3530 on BLSP1 QUP3, enable on gpio 108. */
	.backlight = &backlight_lm3530,
	.backlight_pins = { .i2c = { .bus = 3, .sda = 10, .scl = 11, .func = 3, .addr = 0x38 }, .enable = 108 },
	/* The motor driver takes GP1's duty cycle on gpio 34 (function 3) and
	 * its enable on gpio 62; its supply is L28, which touch keeps on. */
	.motor = &motor_gp_pwm,
	.motor_pins = { .enable = 62, .pwm_gpio = 34, .pwm_func = 3, .pwm_n = 127, .pwm_percent = 80 },
};
