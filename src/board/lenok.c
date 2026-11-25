/* LG G Watch R (lenok), the retail rev-10 board. The PM8226 charges the
 * cell but its battery monitor is unused: a bq27421 measures it. */
#include "asteroid.h"
#include "display/display.h"
#include "soc/bq27421.h"

/* LG4237: the vendor's ambient mode of its Nougat build, about 40 nit at
 * 30 Hz, set by the luminance in the DB step. */
static const struct dcs_step idle_on[] = {
	{ 0x51, 2, 0, { 0x00, 0x00 } },
	{ 0xb0, 1, 0, { 0xac } },
	{ 0xdb, 23, 0, { 0x01, 0xa9, 0x80, 0x93, 0xff, 0x11, 0x1c, 0x34, 0x43, 0x2f, 0x84, 0x85, 0x40, 0x67, 0x27, 0x28,
			 0x12, 0x12, 0x12, 0x00, 0x00, 0x29, 0x10 } },
	{ 0x39, 0, 128, { 0 } },
	{ 0x29, 0, 64, { 0 } },
};
static const struct dcs_step idle_off[] = {
	{ 0x38, 0, 0, { 0 } },
	{ 0x51, 2, 0, { 0x00, 0x00 } },
	{ 0x51, 2, 0, { 0x0a, 0x00 } },
};

/* On BLSP1 QUP2, where the ADSP ran them: the MPU-6515 comes out of
 * reset running, so it is put to sleep. */
static const struct board_chip_off chips_off[] = {
	{ 0x68, 0x6b, 0x00, 0x40, 0, 0xff },
	{ 0 },
};

/* The vendor's battery thermistor table. */
static const struct board_therm therm[] = {
	{ -300, 1627 }, { -200, 1507 }, { -100, 1345 }, { 0, 1159 }, { 100, 972 }, { 200, 807 }, { 250, 737 },
	{ 300, 675 }, { 400, 578 }, { 500, 509 }, { 600, 461 }, { 700, 427 }, { 790, 407 },
};

/* 410 mAh: 4.35 V, 400 mA (the vendor's 410 is off the charger's steps),
 * 30 mA termination. */
BOARD_CHG_CHECK(4350, 4350, 4450, 400, 450);

/* The Bluetooth patch and the GPU microcode are under vendor. */
static const char *const firmware_dirs[] = { "/vendor/firmware/", NULL };

const struct board board_lenok = {
	.name = "lenok",
	.compatible = "lge,lenok",
	.smem_platform = 132,
	.chips_off = { { .bus = 2, .sda = 6, .scl = 7, .func = 3 }, chips_off },
	.heart_rate = true,
	/* LG4237 320x320 command-mode OLED, set up by the bootloader: 328
	 * lines with the porches at 60 Hz, TE on gpio 24. */
	.panel = {
		.width = 320, .height = 320,
		.lines = 328, .hz = 60, .te_gpio = 24, .flip_ud = true, .round = true, .brightness_wide = true,
		.idle_on = idle_on, .idle_on_n = sizeof idle_on / sizeof idle_on[0],
		.idle_off = idle_off, .idle_off_n = sizeof idle_off / sizeof idle_off[0],
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
	/* The WLAN half of the BCM43430 off (WL_REG_ON) and the unused WCD9302
	 * audio codec in reset. */
	.gpios_low = { 46, 72 },
	.pmic_mpm = 62,
	.chg = { .vdd_max_mv = 4350, .vdd_safe_mv = 4350, .vin_min_mv = 4450,
		 .ibat_max_ma = 400, .ibat_safe_ma = 450, .iterm_ma = 30, .no_thermistor = true, .step_down_mv = 4215 },
	.rsense_uohm = 10000,
	/* bq27421 on BLSP1 QUP1. */
	.battery_driver = &battery_pmic_gauge,
	.fuel_gauge = &gauge_bq27421,
	.therm = therm,
	.therm_n = sizeof therm / sizeof therm[0],
	.gauge = { .i2c = { .bus = 1, .sda = 2, .scl = 3, .func = 3, .addr = BQ27421_ADDR } },
	.usb_phy_init = { 0x44, 0x68, 0x24, 0x13 },
	/* Synaptics RMI4 on BLSP1 QUP5 as on dory, fed by L15 and L22. */
	.touch = &touch_rmi4,
	.touch_pins = { .i2c = { .bus = 5, .sda = 18, .scl = 19, .func = 3, .addr = 0x20 }, .irq = 17, .reset = 16, .mpm = 8,
			.ldo = 15, .ldo_uv = 3300000, .ldo_io = 22, .ldo_io_uv = 1800000, .palm_width = 13 },
	/* No hub: the MPU-6515 and the AK8963 behind it on BLSP1 QUP2, its
	 * interrupt on gpio 35 (MPM pin 14), fed by L19. */
	.hub = &hub_mpu6515,
	.hub_pins = { .i2c = { .bus = 2, .sda = 6, .scl = 7, .func = 3, .addr = 0x68 }, .irq = 35, .mpm = 14,
		      .ldo = 19, .ldo_uv = 3000000 },
	/* The motor driver takes GP0's duty cycle on gpio 33 (function 3) and
	 * its enable on gpio 62; its supply is L28, switched with it. */
	.motor = &motor_gp_pwm,
	.motor_pins = { .enable = 62, .pwm_gpio = 33, .pwm_func = 3, .pwm_n = 127, .pwm_percent = 80, .pwm_gp0 = true,
			.ldo = 28, .ldo_uv = 3000000 },
	/* PAH8001 on the same bus: gpio 64 high to use it, gpio 65 high
	 * powers it down. */
	.hr_pins = { .addr = 0x33, .power = 64, .pd = 65 },
};
