/* Moto 360 2015 (smelt). The PM8226 charger and battery monitor are not
 * used on this watch: the vendor disables them, a wireless receiver
 * charges the cell and a separate gauge measures it, so nothing here may
 * point at battery_pmic or give the charger limits. */
#include "asteroid.h"
#include "display/display.h"
#include "soc/max17050.h"

/* The FW3L cell as the vendor's gauge driver programs it; the watch has
 * no battery ID, so this is the profile it falls back to. */
static const struct max17050_model cell_model = {
	.config = 0x4254, .learn_cfg = 0x2606, .filter_cfg = 0x87a4, .relax_cfg = 0x043b, .full_soc_thresh = 0x5f00,
	.design_cap = 776, .fullcap = 776, .fullcapnom = 776, .ichgt_term = 0x0080,
	.rcomp0 = 0x0070, .tempco = 0x5040, .qrtbl = { 0x2f80, 0x1500, 0x0783, 0x0507 },
	.tgain = 0xe71c, .toff = 0x251a, .version = 0x0002,
	.cell = {
		0x7d00, 0xac70, 0xb760, 0xb940, 0xba50, 0xbca0, 0xbde0, 0xc020, 0xc190, 0xc320, 0xc6b0, 0xcac0,
		0xce60, 0xd0d0, 0xd5b0, 0xdba0, 0x0020, 0x0300, 0x1100, 0x0ce0, 0x0fe0, 0x1490, 0x11d0, 0x0c90,
		0x0df0, 0x0970, 0x0600, 0x08d0, 0x0690, 0x06f0, 0x05f0, 0x05f0, 0x0080, 0x0080, 0x0080, 0x0080,
		0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080,
	},
};

/* NT35350 on the large watch: its idle settings are on command page
 * 0x24, and page 0x10 is the normal one. */
static const struct dcs_step idle_on[] = {
	{ 0xff, 1, 0, { 0x24 } }, { 0xd2, 1, 0, { 0xc2 } }, { 0xd3, 1, 0, { 0xc2 } },
	{ 0xd5, 1, 0, { 0x03 } }, { 0xd8, 1, 0, { 0xc3 } }, { 0xff, 1, 0, { 0x10 } },
};
static const struct dcs_step idle_off[] = {
	{ 0xff, 1, 0, { 0x24 } }, { 0xd2, 1, 0, { 0xe1 } }, { 0xd3, 1, 0, { 0xe1 } },
	{ 0xd5, 1, 0, { 0x01 } }, { 0xd8, 1, 0, { 0x43 } }, { 0xff, 1, 0, { 0x10 } },
};

/* The same panel from reset: its settings, the 360 x 330 window, tearing
 * effect on and out of sleep. */
static const struct dcs_step panel_on[] = {
	{ 0xff, 1, 0, { 0x24 } }, { 0xfb, 1, 0, { 0x01 } }, { 0xc8, 1, 0, { 0x00 } },
	{ 0xd2, 1, 0, { 0xe1 } }, { 0xd3, 1, 0, { 0xe1 } }, { 0xd4, 1, 0, { 0xc2 } },
	{ 0xd5, 1, 0, { 0x01 } }, { 0xd8, 1, 0, { 0x43 } },
	{ 0xff, 1, 0, { 0x10 } }, { 0xfb, 1, 0, { 0x01 } },
	{ 0x2a, 4, 0, { 0x00, 0x00, 0x01, 0x67 } }, { 0x2b, 4, 0, { 0x00, 0x00, 0x01, 0x49 } },
	{ 0xc0, 1, 0, { 0x01 } }, { 0x35, 1, 0, { 0x00 } }, { 0x11, 0, 100, { 0 } },
};

/* The Bluetooth patch, GPU microcode and the rest are all under vendor. */
static const char *const firmware_dirs[] = { "/vendor/firmware/", NULL };

const struct board board_smelt = {
	.name = "smelt",
	.compatible = "moto,smelt",
	.smem_platform = 0x41,
	.heart_rate = false,
	/* NT35350 or NT35355 command-mode panel, a round 360 px with the
	 * bottom cut off: 330 lines on the large watch, 325 on the small
	 * one, 401 with the porches at 60 Hz, TE on gpio 24. The small
	 * watch has one of two panels with idle commands of their own and
	 * nothing here tells those apart yet, so only the large one's
	 * always-on face runs in idle mode and only the large one is
	 * switched off with the screen. */
	.panel = {
		.width = 360, .height = 330, .height_alt = 325,
		.lines = 401, .hz = 60, .te_gpio = 24, .flip_ud = false, .round = true,
		.lcd = true, .edge_px = 72,
		.idle_on = idle_on, .idle_on_n = sizeof idle_on / sizeof idle_on[0],
		.idle_off = idle_off, .idle_off_n = sizeof idle_off / sizeof idle_off[0],
		.on = panel_on, .on_n = sizeof panel_on / sizeof panel_on[0],
		.reset_gpio = 36, .bias_gpio = 33, .io_ldo = 27, .io_uv = 1800000,
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
	.gauge = { .i2c = { .bus = 3, .sda = 10, .scl = 11, .func = 3, .addr = 0x36 }, .dock = 54, .dock_mpm = 25, .charge_n = 94, .charge_term = 53, .charge_rate = 58,
		   .kill_disable = 116, .model = &cell_model },
	/* Atmel mXT on BLSP1 QUP5, interrupt on gpio 17 (MPM pin 8), reset
	 * on gpio 16. */
	.touch = &touch_mxt,
	.touch_pins = { .i2c = { .bus = 5, .sda = 18, .scl = 19, .func = 3, .addr = 0x4a }, .irq = 17, .reset = 16, .mpm = 8, .ldo = 28, .ldo_uv = 2800000 },
	/* M4 hub on BLSP1 QUP2, wake line on gpio 5 (MPM pin 5), reset on
	 * gpio 62, boot0 on gpio 109. */
	.hub = &hub_m4,
	.hub_pins = { .i2c = { .bus = 2, .sda = 6, .scl = 7, .func = 3, .addr = 0x43 }, .irq = 5, .reset = 62, .boot0 = 109, .mpm = 5 },
	/* LM3535 on BLSP1 QUP3 next to the gauge, enable on gpio 35. */
	.backlight = &backlight_lm3535,
	.backlight_pins = { .i2c = { .bus = 3, .sda = 10, .scl = 11, .func = 3, .addr = 0x38 }, .enable = 35 },
	/* The vendor's ambient maximum, below the curve's first steps, and
	 * what the always-on face uses without a light reading. */
	.backlight_aod_code = 36,
	/* TSL2584 on BLSP1 QUP3 too, fed by L18. The coefficients are the
	 * vendor's for the large and the small watch. */
	.light = &light_tsl2584,
	.light_pins = { .i2c = { .bus = 3, .sda = 10, .scl = 11, .func = 3, .addr = 0x29 }, .ldo = 18, .ldo_uv = 2950000,
			.coeff = { { 1627, 2485, 1536, 2259 }, { 1932, 3054, 1726, 2539 } } },
	/* The PM8226 vibrator. */
	.motor = &motor_pm8226,
	.motor_pins = { .vib_mv = 2300 },
};
