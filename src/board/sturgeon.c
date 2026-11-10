/* Huawei Watch (sturgeon). */
#include "asteroid.h"
#include "display/display.h"

/* RM69080: entering idle mode selects command page 0 and sends a NOP
 * followed by 150 ms before the idle command. */
static const struct dcs_step idle_on[] = {
	{ 0xfe, 1, 0, { 0x00 } },
	{ 0x00, 1, 150, { 0x00 } },
	{ 0x39, 0, 0, { 0 } },
};
static const struct dcs_step idle_off[] = {
	{ 0xfe, 1, 0, { 0x00 } },
	{ 0x38, 0, 0, { 0 } },
};

/* The HB442528EBC cell, from either of two makers told apart by the ID
 * resistor; they share the voltage table but not the internal
 * resistance. */
static const struct battery_cell cells[] = {
	{ "SCUD", 10, 496, {
		{ 1926, 401, 100, 65, 47 }, { 1916, 401, 100, 65, 47 }, { 1767, 405, 102, 67, 48 },
		{ 1681, 407, 105, 69, 49 }, { 1650, 414, 110, 72, 52 }, { 1598, 422, 116, 76, 54 },
		{ 1570, 415, 124, 82, 58 }, { 1567, 396, 133, 90, 63 }, { 1568, 383, 122, 85, 61 },
		{ 1585, 374, 103, 71, 52 }, { 1616, 376, 99, 66, 49 }, { 1658, 383, 101, 68, 51 },
		{ 1711, 395, 104, 73, 54 }, { 1768, 415, 108, 78, 58 }, { 1855, 441, 110, 73, 50 },
		{ 1954, 470, 105, 67, 48 }, { 2070, 501, 105, 68, 49 }, { 2070, 503, 105, 67, 48 },
		{ 2171, 529, 103, 65, 47 }, { 2276, 565, 107, 68, 50 }, { 2336, 589, 111, 72, 51 },
		{ 2474, 617, 116, 75, 53 }, { 2673, 649, 124, 77, 54 }, { 2990, 690, 126, 76, 54 },
		{ 3396, 719, 126, 78, 59 }, { 3953, 764, 136, 89, 72 }, { 4767, 834, 157, 108, 84 },
		{ 6044, 932, 179, 117, 78 }, { 8223, 1079, 191, 120, 87 }, { 14102, 1430, 241, 166, 142 },
		{ 28204, 2860, 482, 332, 284 },
	} },
	{ "XWD", 470, 487, {
		{ 2059, 415, 100, 64, 46 }, { 2053, 415, 100, 64, 46 }, { 1925, 416, 101, 65, 46 },
		{ 1843, 416, 104, 67, 47 }, { 1808, 421, 107, 69, 49 }, { 1755, 431, 113, 73, 51 },
		{ 1730, 422, 120, 77, 54 }, { 1725, 408, 128, 83, 58 }, { 1722, 400, 127, 86, 60 },
		{ 1732, 392, 111, 77, 54 }, { 1762, 390, 101, 67, 48 }, { 1810, 394, 101, 68, 49 },
		{ 1874, 405, 105, 71, 53 }, { 1958, 425, 108, 76, 56 }, { 2067, 455, 112, 72, 52 },
		{ 2198, 492, 109, 67, 48 }, { 2345, 527, 107, 68, 49 }, { 2431, 556, 108, 67, 48 },
		{ 2347, 575, 105, 65, 47 }, { 2534, 622, 111, 69, 50 }, { 2752, 653, 115, 72, 51 },
		{ 3012, 689, 120, 75, 52 }, { 3324, 730, 127, 77, 53 }, { 3739, 767, 129, 77, 53 },
		{ 4292, 808, 130, 78, 58 }, { 5049, 867, 141, 89, 70 }, { 6129, 957, 162, 109, 86 },
		{ 7924, 1086, 197, 127, 86 }, { 11469, 1426, 233, 137, 92 }, { 22938, 2852, 344, 206, 128 },
		{ 45877, 5703, 688, 411, 255 },
	} },
};

static const struct board_battery battery = {
	.lut_temp = { -20, 0, 25, 40, 60 },
	.lut_pc = { 100, 95, 90, 85, 80, 75, 70, 65, 60, 55, 50, 45, 40, 35, 30, 25,
		     20, 16, 13, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 },
	.fcc_mah = { 322, 321, 322, 321, 320 },
	.ocv_mv = {
		{ 4351, 4348, 4344, 4340, 4334 }, { 4253, 4275, 4279, 4277, 4272 }, { 4182, 4215, 4222, 4220, 4216 },
		{ 4121, 4158, 4166, 4164, 4160 }, { 4067, 4107, 4112, 4112, 4108 }, { 4000, 4056, 4062, 4061, 4058 },
		{ 3942, 4000, 4014, 4014, 4011 }, { 3900, 3948, 3970, 3970, 3967 }, { 3864, 3905, 3922, 3924, 3922 },
		{ 3836, 3868, 3878, 3878, 3877 }, { 3817, 3838, 3846, 3846, 3845 }, { 3802, 3814, 3822, 3822, 3821 },
		{ 3788, 3795, 3802, 3802, 3800 }, { 3775, 3782, 3784, 3784, 3782 }, { 3760, 3770, 3769, 3764, 3754 },
		{ 3744, 3756, 3750, 3742, 3728 }, { 3726, 3735, 3728, 3719, 3706 }, { 3708, 3712, 3704, 3694, 3682 },
		{ 3691, 3694, 3685, 3678, 3667 }, { 3676, 3686, 3682, 3674, 3664 }, { 3668, 3682, 3679, 3672, 3661 },
		{ 3658, 3678, 3676, 3670, 3658 }, { 3648, 3672, 3672, 3664, 3650 }, { 3632, 3663, 3660, 3648, 3628 },
		{ 3610, 3642, 3627, 3615, 3594 }, { 3579, 3602, 3586, 3573, 3558 }, { 3536, 3550, 3539, 3528, 3514 },
		{ 3475, 3488, 3484, 3472, 3454 }, { 3392, 3410, 3406, 3389, 3364 }, { 3274, 3298, 3283, 3260, 3229 },
		{ 3000, 3000, 3000, 3000, 3000 },
	},
	.cutoff_uv = 3200000,
	.low_uv = 3350000,
	.cells = cells,
	.ncells = sizeof cells / sizeof cells[0],
};

static const char *const firmware_dirs[] = { "/vendor/firmware/", NULL };

/* 300 mAh: 4.35 V, 1 C fast charge, 20 mA termination. */
BOARD_CHG_CHECK(4350, 4430, 4350, 300, 600);

const struct board board_sturgeon = {
	.name = "sturgeon",
	.compatible = "huawei,sturgeon",
	.heart_rate = true,
	/* RM69080 400x400 command-mode panel: 434 lines with the porches at
	 * 60 Hz, TE into the MDP, mounted upside down. */
	.panel = {
		.width = 400, .height = 400,
		.lines = 434, .hz = 60, .te_gpio = 24, .flip_ud = true, .round = true,
		.idle_on = idle_on, .idle_on_n = sizeof idle_on / sizeof idle_on[0],
		.idle_off = idle_off, .idle_off_n = sizeof idle_off / sizeof idle_off[0],
	},
	.firmware_dirs = firmware_dirs,
	/* BCM43430A0 on BLSP1 UART4 (gpio 12-15, function 2). Both wake lines
	 * are active low. Sleep mode: mode 1, host and controller idle
	 * thresholds 0x18, dev-wake and host-wake polarity 0, then the
	 * SCO/LPM/tristate flags. */
	.bt = {
		.uart = 4, .gpio = 12, .func = 2, .dev_wake = 63, .host_wake = 64, .reg_on = 67, .mpm = 28,
		.dev_wake_active = false, .host_wake_active = false,
		.patch = "4343A0.hcd",
		.sleep_mode = { 1, 0x18, 0x18, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
	},
	.pmic_mpm = 62,
	.chg = { .vdd_max_mv = 4350, .vdd_safe_mv = 4430, .vin_min_mv = 4350,
		 .ibat_max_ma = 300, .ibat_safe_ma = 600, .iterm_ma = 20 },
	.rsense_uohm = 9700,
	.usb_phy_init = { 0x44, 0x68, 0x3c, 0xf3 },
	.usb_rails_off = true,
	.unused_ldos = { 18, 21 },
	.battery = &battery,
	.battery_driver = &battery_pmic,
	/* Synaptics RMI4 on BLSP1 QUP5. */
	.touch = &touch_rmi4,
	.touch_pins = { .i2c = { .bus = 5, .sda = 18, .scl = 19, .func = 3, .addr = 0x20 }, .irq = 17, .reset = 16, .mpm = 8 },
	/* STM32 sensor hub on BLSP1 UART1 (gpio 0-3, function 2). */
	.hub = &hub_stm32,
	.hub_pins = {
		.uart = 1, .gpio = 0, .func = 2, .wake_mcu = 62, .mcu_wake = 65, .reset = 87,
		.boot1 = 106, .ap_status = 108, .boot0 = 111, .mcu_status = 114, .mpm = 29,
	},
	/* DRV2605 on BLSP1 QUP2. */
	.motor = &motor_drv2605,
	.motor_pins = { .i2c = { .bus = 2, .sda = 6, .scl = 7, .func = 3, .addr = 0x5a }, .trigger = 59, .enable = 60 },
	/* TFA9895 on BLSP1 QUP3. */
	.speaker = { .bus = 3, .sda = 10, .scl = 11, .func = 3, .addr = 0x34 },
};
