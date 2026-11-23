/* ASUS ZenWatch 3 (swift): APQ8009W with a PM8916, the production (PR)
 * board. Bluetooth is the SoC's own WCNSS, which nothing here starts. */
#include "asteroid.h"
#include "display/display.h"
#include "soc/stc3117.h"

/* SMB231 as stock leaves it on a computer's USB port: 300 mA input, 20 mA
 * termination, 250 mA fast charge, recharge 150 mV below 4.4 V, a 360 min
 * safety timer, hard temperature limits at 5 and 60 C and soft ones at
 * 15 and 45 C. Recharge and termination are enabled explicitly. */
static const struct board_reg charger_setup[] = {
	{ 0x00, 0x1f, 1 << 2 | 0 },
	{ 0x02, 1 << 7 | 1 << 6 | 1 << 5 | 7, 1 },
	{ 0x03, 0xff, (4400 - 3480) / 40 },
	{ 0x04, 1 << 5 | 3 << 3 | 3, 2 << 3 | 2 },
	{ 0x05, 0xff, 3 << 2 | 1 },
	{ 0x06, 3 << 6 | 3 << 4, 2 << 4 },
	{ 0x07, 0xe8, 0x08 },
	{ 0x08, 0, 1 << 6 | 2 << 4 | 0 << 2 | 1 },
};

/* The STC3117's cell: the tree's numbers and the vendor's tables. */
static const struct stc3117_cell cell = {
	.cnom_mah = 340, .rsense_mohm = 10, .rint_mohm = 501, .cc_cnf = 66, .vm_cnf = 184,
	.alm_soc = 10, .alm_mv = 3600, .relax_ma = 17,
	.derating = { 0, 0, 0, 10, 37, 37, 37 },
	.ocv_mv = { 3400, 3605, 3672, 3687, 3704, 3734, 3755, 3779, 3818, 3866, 3947, 3999, 4053, 4160, 4226, 4390 },
	.soc = { 0x00, 0x06, 0x0c, 0x14, 0x1e, 0x28, 0x32, 0x3c, 0x50, 0x64, 0x78, 0x82, 0x8c, 0xa0, 0xb4, 0xc8 },
};

/* The vendor's on-commands up to sleep out, sent again before each wake
 * in case the panel lost them asleep: a page 1 register, TE on and
 * brightness control on. */
static const struct dcs_step panel_wake[] = {
	{ 0xfe, 1, 0, { 0x01 } }, { 0x0a, 1, 0, { 0xf0 } }, { 0xfe, 1, 0, { 0x00 } },
	{ 0x35, 1, 0, { 0x00 } }, { 0x53, 1, 0, { 0x20 } },
};
/* AsteroidOS's automatic brightness for swift, from its mce settings:
 * the same lux steps as the default, but never under 8 %. */
static const uint8_t brightness_levels[21][21] = {
	{ 8, 9, 11, 13, 15, 17, 19, 21, 22, 24, 25, 33, 40, 47, 55, 62, 70, 77, 85, 92, 100 },
	{ 8, 10, 12, 14, 16, 18, 21, 22, 24, 26, 28, 35, 43, 50, 57, 64, 72, 78, 86, 93, 100 },
	{ 8, 10, 12, 15, 17, 20, 21, 24, 26, 29, 31, 38, 45, 52, 59, 65, 73, 79, 86, 93, 100 },
	{ 8, 10, 13, 16, 18, 21, 23, 25, 28, 31, 33, 40, 47, 54, 60, 67, 73, 80, 86, 93, 100 },
	{ 8, 10, 13, 16, 19, 22, 24, 27, 30, 33, 36, 42, 49, 55, 62, 68, 74, 81, 87, 94, 100 },
	{ 8, 11, 14, 17, 21, 23, 26, 30, 33, 36, 38, 45, 51, 57, 63, 69, 75, 82, 87, 94, 100 },
	{ 8, 11, 14, 18, 21, 25, 27, 31, 34, 38, 41, 47, 53, 59, 64, 70, 77, 82, 88, 94, 100 },
	{ 8, 11, 15, 19, 23, 26, 29, 33, 37, 41, 44, 49, 55, 61, 66, 72, 78, 83, 89, 95, 100 },
	{ 8, 11, 15, 19, 23, 27, 31, 34, 38, 42, 46, 52, 57, 62, 68, 73, 78, 84, 89, 95, 100 },
	{ 8, 12, 16, 20, 25, 29, 32, 37, 41, 45, 49, 54, 59, 64, 70, 75, 79, 85, 90, 95, 100 },
	{ 8, 12, 16, 21, 25, 30, 34, 38, 42, 47, 51, 56, 61, 66, 70, 75, 80, 85, 90, 95, 100 },
	{ 8, 12, 17, 22, 26, 31, 36, 40, 45, 50, 54, 58, 63, 68, 72, 77, 82, 86, 91, 96, 100 },
	{ 8, 12, 17, 22, 27, 32, 37, 42, 47, 52, 56, 61, 65, 70, 74, 78, 83, 87, 92, 96, 100 },
	{ 8, 14, 19, 24, 29, 34, 39, 44, 49, 54, 59, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100 },
	{ 10, 16, 21, 26, 31, 36, 42, 47, 52, 57, 62, 66, 70, 74, 78, 81, 85, 89, 93, 97, 100 },
	{ 11, 17, 22, 28, 33, 38, 44, 49, 55, 60, 65, 69, 72, 76, 79, 83, 86, 90, 93, 97, 100 },
	{ 13, 19, 24, 30, 35, 41, 46, 52, 57, 63, 68, 72, 75, 78, 81, 84, 88, 91, 94, 97, 100 },
	{ 15, 21, 27, 32, 38, 43, 49, 55, 60, 66, 71, 74, 77, 80, 83, 86, 89, 92, 95, 98, 100 },
	{ 18, 24, 30, 35, 41, 46, 52, 58, 63, 69, 74, 77, 80, 82, 85, 87, 90, 93, 95, 98, 100 },
	{ 21, 27, 33, 38, 44, 49, 55, 61, 66, 72, 77, 80, 82, 84, 87, 89, 91, 94, 96, 98, 100 },
	{ 25, 31, 36, 42, 47, 53, 58, 64, 69, 75, 80, 82, 84, 86, 88, 90, 92, 94, 96, 98, 100 },
};
/* The GPU microcode is under vendor on the system partition. */
static const char *const firmware_dirs[] = { "/vendor/firmware/", NULL };
/* The vendor's ambient mode, through the panel's command pages. */
static const struct dcs_step idle_on[] = {
	{ 0xfe, 1, 0, { 0x01 } },
	{ 0x30, 1, 0, { 0x41 } },
	{ 0xfe, 1, 0, { 0x00 } },
	{ 0x39, 0, 100, { 0 } },
};
static const struct dcs_step idle_off[] = {
	{ 0xfe, 1, 0, { 0x00 } },
	{ 0x51, 1, 0, { 0x00 } },
	{ 0xfe, 1, 0, { 0x01 } },
	{ 0x30, 1, 0, { 0x40 } },
	{ 0xfe, 1, 17, { 0x00 } },
	{ 0x38, 0, 25, { 0 } },
	{ 0x51, 1, 0, { 0x1a } },
};

const struct board board_swift = {
	.name = "swift",
	.compatible = "asus,swift",
	.smem_subtype = 5,
	.bt = { .wcnss = true },
	/* AUO H139BLN01, 400x400 AMOLED in command mode on one lane, TE
	 * into the MDP. */
	.panel = {
		.width = 400, .height = 400,
		.hz = 60, .te_gpio = 24, .round = true, .reset_gpio = 25, .brightness_levels = brightness_levels,
		.lp_commands = true,
		.wake = panel_wake, .wake_n = sizeof panel_wake / sizeof panel_wake[0],
		.idle_on = idle_on, .idle_on_n = sizeof idle_on / sizeof idle_on[0],
		.idle_off = idle_off, .idle_off_n = sizeof idle_off / sizeof idle_off[0],
	},
	.firmware_dirs = firmware_dirs,
	.pmic_mpm = 62,
	.key_reset_ms = 6720,
	.crash_to_fastboot = true,
	/* fs/pstore/ram_dev.c of its kernel. */
	.aos_ramoops = { .phys = 0x8fe00000, .size = 0x200000, .record_size = 0x10000, .console_size = 0x10000,
			 .ftrace_size = 0x1000 },
	/* The top button is gpio 90, the middle one the PMIC's power key and
	 * the bottom one gpio 91; all three act as the power key. RESIN is
	 * on none of them, but stock sets it up as a key. */
	.keys = { { .gpio = 90, .mpm = 30 }, { .gpio = 91, .mpm = 32 } },
	.resin_key = true,
	.usb_phy_init = { 0x73, 0x33, 0x07, 0x13 },
	/* HX852xES on BLSP1 QUP5, fed by L18. */
	.touch = &touch_hx852x,
	.touch_pins = { .i2c = { .bus = 5, .sda = 18, .scl = 19, .func = 2, .addr = 0x48 }, .range = 400,
			.irq = 13, .reset = 12, .mpm = 10, .ldo = 18, .ldo_uv = 2700000 },
	/* No hub: an LSM6DS3 on BLSP1 QUP3 as SPI (gpio 0-3, function 1, chip
	 * select gpio 2), INT1 on gpio 96. */
	.hub = &hub_lsm6ds3,
	.hub_pins = { .spi = 3, .gpio = 0, .func = 1, .cs = 2, .irq = 96, .mpm = 36 },
	/* AL3320B on BLSP1 QUP1. */
	.light = &light_al3320,
	.light_pins = { .i2c = { .bus = 1, .sda = 6, .scl = 7, .func = 3, .addr = 0x1c } },
	/* SMB231 and STC3117 on BLSP1 QUP4, the charger's STAT line on gpio 49.
	 * Like stock on a computer's port, the PM8916's own charger stays
	 * suspended and gpio 17 low. Stock asleep keeps only the missing
	 * battery interrupt; the input one stays on here for a plug-in. */
	.battery_driver = &battery_smb231,
	.charger = { .i2c = { .bus = 4, .sda = 14, .scl = 15, .func = 2, .addr = 0x12 }, .irq = 49, .mpm = 11, .suspend = 58,
		     .setup = charger_setup, .setup_n = sizeof charger_setup / sizeof charger_setup[0],
		     .irqs_awake = 0xbb, .irqs_asleep = 0x21, .cmd1 = 1, .cell_mah = 340, .hold_low = 17,
		     .pmic_charger_off = true, .ov_mv = 4420, .ov_float_mv = 4360,
		     .empty_mv = 3250 },
	.fuel_gauge = &gauge_stc3117,
	.gauge = { .i2c = { .bus = 4, .sda = 14, .scl = 15, .func = 2, .addr = STC3117_ADDR }, .cell = &cell },
	.motor = &motor_pm8226,
	.motor_pins = { .vib_mv = 2000 },
};
