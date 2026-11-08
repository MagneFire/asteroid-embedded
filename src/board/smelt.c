/* Moto 360 2015 (smelt). The PM8226 charger and battery monitor are not
 * used on this watch: the vendor disables them, a wireless receiver
 * charges the cell and a separate gauge measures it, so nothing here may
 * point at battery_pmic or give the charger limits. */
#include "asteroid.h"

/* The Bluetooth patch, GPU microcode and the rest are all under vendor. */
static const char *const firmware_dirs[] = { "/vendor/firmware/", NULL };

const struct board board_smelt = {
	.name = "smelt",
	.compatible = "moto,smelt",
	.smem_platform = 0x41,
	.heart_rate = false,
	/* NT35350 or NT35355 command-mode panel, a round 360 px with the
	 * bottom cut off: 330 lines on the large watch, 325 on the small
	 * one, 401 with the porches at 60 Hz, TE on gpio 24. The idle
	 * commands differ between the three panels and nothing here tells
	 * them apart yet, so the always-on face runs in normal mode. */
	.panel = {
		.width = 360, .height = 330, .height_alt = 325,
		.lines = 401, .hz = 60, .te_gpio = 24, .flip_ud = false, .round = true,
		.lcd = true, .edge_px = 72,
	},
	.firmware_dirs = firmware_dirs,
	/* BCM43430A1 on BLSP1 UART6 (gpio 20-23, function 2), REG_ON on gpio
	 * 26, host wake on gpio 27 (MPM pin 10), device wake on gpio 30.
	 * Both wake lines are active low. Sleep mode as sturgeon's, which
	 * has the same polarity. */
	.bt = {
		.uart = 6, .gpio = 20, .func = 2, .dev_wake = 30, .host_wake = 27, .reg_on = 26, .mpm = 10,
		.dev_wake_active = false, .host_wake_active = false,
		.patch = "BCM43430A1_001.002.009.0031.0000_TEST.hcd",
		.sleep_mode = { 1, 0x18, 0x18, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
	},
	.pmic_mpm = 62,
	.usb_phy_init = { 0x44, 0x68, 0x24, 0x13 },
	.rsense_uohm = 10000,
	/* MAX17050 on BLSP1 QUP3, pad detect on gpio 54 (MPM pin 25). */
	.battery_driver = &battery_max17050,
	.gauge = { .i2c = { .qup = 3, .sda = 10, .scl = 11, .func = 3, .addr = 0x36 }, .dock = 54, .dock_mpm = 25, .charge_n = 94, .charge_term = 53, .charge_rate = 58,
		   .kill_disable = 116 },
	/* Atmel mXT on BLSP1 QUP5, interrupt on gpio 17 (MPM pin 8), reset
	 * on gpio 16. */
	.touch = &touch_mxt,
	.touch_pins = { .i2c = { .qup = 5, .sda = 18, .scl = 19, .func = 3, .addr = 0x4a }, .irq = 17, .reset = 16, .mpm = 8 },
	/* M4 hub on BLSP1 QUP2, wake line on gpio 5 (MPM pin 5), reset on
	 * gpio 62, boot0 on gpio 109. */
	.hub = &hub_m4,
	.hub_pins = { .i2c = { .qup = 2, .sda = 6, .scl = 7, .func = 3, .addr = 0x43 }, .irq = 5, .reset = 62, .boot0 = 109, .mpm = 5 },
	/* LM3535 on BLSP1 QUP3 next to the gauge, enable on gpio 35. */
	.backlight = &backlight_lm3535,
	.backlight_pins = { .i2c = { .qup = 3, .sda = 10, .scl = 11, .func = 3, .addr = 0x38 }, .enable = 35 },
	/* The vendor's ambient maximum, below the curve's first steps, and
	 * what the always-on face uses without a light reading. */
	.backlight_aod_code = 36,
	/* TSL2584 on BLSP1 QUP3 too, fed by L18. The coefficients are the
	 * vendor's for the large and the small watch. */
	.light = &light_tsl2584,
	.light_pins = { .i2c = { .qup = 3, .sda = 10, .scl = 11, .func = 3, .addr = 0x29 }, .ldo = 18, .ldo_uv = 2950000,
			.coeff = { { 1627, 2485, 1536, 2259 }, { 1932, 3054, 1726, 2539 } } },
	/* The PM8226 vibrator. */
	.motor = &motor_pm8226,
	.motor_pins = { .vib_mv = 2300 },
};
