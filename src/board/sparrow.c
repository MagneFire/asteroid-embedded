/* ASUS ZenWatch 2 (sparrow), the PR3 revision on the bench. The power
 * key is read through the PMIC's PON block like sturgeon's, so the
 * gpio-keys line (gpio 37) is unused. */
#include "asteroid.h"
#include "display/display.h"

/* RM69032: the idle sequences the watch's own software uses for its
 * ambient mode, through the vendor command pages. */
static const struct dcs_step idle_on[] = {
	{ 0xf0, 5, 0, { 0x55, 0xaa, 0x52, 0x08, 0x02 } },
	{ 0xed, 3, 50, { 0x48, 0x00, 0xf0 } },
	{ 0xf0, 5, 0, { 0x55, 0xaa, 0x52, 0x08, 0x01 } },
	{ 0xc2, 12, 85, { 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02 } },
	{ 0x39, 0, 0, { 0 } },
	{ 0xb3, 1, 10, { 0x89 } },
	{ 0xc0, 1, 0, { 0x26 } },
};
static const struct dcs_step idle_off[] = {
	{ 0xf0, 5, 0, { 0x55, 0xaa, 0x52, 0x08, 0x01 } },
	{ 0xc0, 1, 0, { 0x28 } },
	{ 0x38, 0, 60, { 0 } },
	{ 0xb3, 1, 10, { 0x00 } },
	{ 0xc2, 12, 0, { 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b } },
	{ 0xf0, 5, 0, { 0x55, 0xaa, 0x52, 0x08, 0x02 } },
	{ 0xed, 3, 50, { 0x48, 0x00, 0xe0 } },
	{ 0xf0, 5, 0, { 0x55, 0xaa, 0x52, 0x08, 0x01 } },
};

/* The ZenWatch 2 takes one of two cells, C11N1540 (380 mAh, 4.35 V, the
 * PR3's) and C11N1502 (400 mAh, 4.40 V); both packs carry the same 75
 * kOhm ID resistor, so only the PR3's is here. The vendor table stops at
 * 1 %; the 0 % row is twice that, as in sturgeon's. */
static const struct battery_cell cells[] = {
	{ "C11N1540", 75, 401, {
		{ 941, 342, 100, 67, 52 }, { 941, 342, 100, 67, 52 }, { 984, 340, 102, 69, 53 },
		{ 999, 336, 104, 71, 54 }, { 1038, 334, 106, 74, 56 }, { 1024, 340, 113, 78, 59 },
		{ 1090, 316, 106, 76, 61 }, { 1135, 318, 110, 80, 63 }, { 1198, 319, 103, 76, 61 },
		{ 1264, 315, 96, 68, 54 }, { 1355, 315, 95, 67, 53 }, { 1473, 316, 96, 68, 55 },
		{ 1625, 320, 98, 71, 58 }, { 1826, 332, 100, 73, 60 }, { 2077, 347, 101, 72, 57 },
		{ 2414, 367, 102, 68, 53 }, { 2836, 394, 104, 69, 53 }, { 3208, 425, 105, 69, 52 },
		{ 3503, 439, 105, 68, 52 }, { 3304, 393, 103, 68, 53 }, { 3296, 391, 104, 69, 54 },
		{ 3543, 401, 106, 71, 55 }, { 3812, 413, 109, 72, 56 }, { 4157, 423, 110, 72, 54 },
		{ 4641, 431, 108, 70, 54 }, { 5205, 471, 111, 72, 58 }, { 5986, 540, 116, 76, 59 },
		{ 7114, 630, 123, 78, 59 }, { 8975, 765, 122, 80, 61 }, { 17029, 1531, 173, 128, 71 },
		{ 34058, 3062, 346, 256, 142 },
	} },
};

static const struct board_battery battery = {
	.lut_temp = { -20, 0, 25, 40, 60 },
	.lut_pc = { 100, 95, 90, 85, 80, 75, 70, 65, 60, 55, 50, 45, 40, 35, 30, 25,
		     20, 16, 13, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 },
	.fcc_mah = { 382, 384, 384, 384, 384 },
	.ocv_mv = {
		{ 4339, 4338, 4333, 4328, 4322 }, { 4232, 4256, 4262, 4260, 4256 }, { 4160, 4199, 4206, 4204, 4201 },
		{ 4114, 4148, 4152, 4151, 4148 }, { 4070, 4099, 4103, 4101, 4098 }, { 3966, 4058, 4064, 4056, 4051 },
		{ 3924, 3968, 3995, 4001, 4006 }, { 3886, 3930, 3962, 3966, 3964 }, { 3854, 3898, 3920, 3924, 3924 },
		{ 3832, 3865, 3877, 3879, 3879 }, { 3814, 3836, 3846, 3848, 3847 }, { 3800, 3812, 3823, 3824, 3824 },
		{ 3786, 3794, 3804, 3805, 3804 }, { 3772, 3782, 3788, 3788, 3786 }, { 3760, 3770, 3774, 3770, 3764 },
		{ 3745, 3756, 3758, 3748, 3735 }, { 3729, 3738, 3737, 3728, 3714 }, { 3712, 3725, 3714, 3704, 3690 },
		{ 3696, 3714, 3697, 3688, 3675 }, { 3683, 3706, 3691, 3683, 3672 }, { 3676, 3701, 3690, 3682, 3670 },
		{ 3665, 3696, 3688, 3680, 3668 }, { 3653, 3690, 3684, 3675, 3662 }, { 3636, 3680, 3672, 3660, 3642 },
		{ 3612, 3656, 3642, 3629, 3610 }, { 3580, 3616, 3601, 3590, 3572 }, { 3534, 3564, 3554, 3544, 3527 },
		{ 3468, 3498, 3496, 3486, 3465 }, { 3373, 3416, 3418, 3402, 3380 }, { 3244, 3293, 3286, 3264, 3246 },
		{ 3000, 3000, 3000, 3000, 3000 },
	},
	.cutoff_uv = 3400000,
	.low_uv = 3420000,
	.cells = cells,
	.ncells = sizeof cells / sizeof cells[0],
};

/* The GPU microcode under vendor, the Bluetooth patch and the sensor hub
 * firmware under etc. */
static const char *const firmware_dirs[] = { "/vendor/firmware/", "/etc/firmware/bt/", "/etc/firmware/", NULL };

/* C11N1540: 4.35 V, 300 mA fast charge (below 1 C), 20 mA termination. */
BOARD_CHG_CHECK(4350, 4380, 4350, 300, 600);

const struct board board_sparrow = {
	.name = "sparrow",
	.compatible = "asus,sparrow",
	.heart_rate = false,
	/* RM69032 320x320 command-mode panel: 326 lines with the porches at
	 * 57 Hz, TE on gpio 24. */
	.panel = {
		.width = 320, .height = 320,
		.lines = 326, .hz = 57, .te_gpio = 24, .flip_ud = false, .round = false,
		.idle_on = idle_on, .idle_on_n = sizeof idle_on / sizeof idle_on[0],
		.idle_off = idle_off, .idle_off_n = sizeof idle_off / sizeof idle_off[0],
	},
	.firmware_dirs = firmware_dirs,
	/* BCM4343A1 on BLSP1 UART1 (gpio 0-3, function 2), REG_ON on gpio 34,
	 * host wake on gpio 48 (MPM pin 20), device wake on gpio 61. Both wake
	 * lines are active high. Sleep mode: mode 1, host and controller idle
	 * thresholds 1, dev-wake and host-wake polarity 1, then the
	 * SCO/LPM/tristate flags. */
	.bt = {
		.uart = 1, .gpio = 0, .func = 2, .dev_wake = 61, .host_wake = 48, .reg_on = 34, .mpm = 20,
		.dev_wake_active = true, .host_wake_active = true,
		.patch = "BCM4343A1_001.002.009.0053.0240.hcd",
		.sleep_mode = { 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 },
	},
	.pmic_mpm = 62,
	.chg = { .vdd_max_mv = 4350, .vdd_safe_mv = 4380, .vin_min_mv = 4350,
		 .ibat_max_ma = 300, .ibat_safe_ma = 600, .iterm_ma = 20 },
	.rsense_uohm = 10000,
	.usb_phy_init = { 0x44, 0x68, 0x34, 0x13 },
	.battery = &battery,
	/* ITE IT7260 on BLSP1 QUP5, interrupt on gpio 17 (MPM pin 8), reset on
	 * gpio 16. */
	.touch = &touch_it7260,
	.touch_pins = { .i2c = { .qup = 5, .sda = 18, .scl = 19, .func = 3, .addr = 0x46 }, .irq = 17, .reset = 16, .mpm = 8 },
	/* PNI EM7184 on BLSP1 QUP2 (gpio 6/7, function 3), interrupt on gpio
	 * 111 (MPM pin 54); its firmware is uploaded at every boot. */
	.hub = &hub_em7184,
	.hub_pins = { .i2c = { .qup = 2, .sda = 6, .scl = 7, .func = 3, .addr = 0x28 }, .irq = 111, .mpm = 54 },
	/* The PM8226 vibrator. */
	.motor = &motor_pm8226,
	.motor_pins = { .vib_mv = 3000 },
};
