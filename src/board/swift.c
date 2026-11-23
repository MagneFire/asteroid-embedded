/* ASUS ZenWatch 3 (swift): APQ8009W with a PM8916, the production (PR)
 * board. Bluetooth is the SoC's own WCNSS, which nothing here starts. */
#include "asteroid.h"
#include "display/display.h"

/* Brightness control on, as the vendor's on-commands have it. */
static const struct dcs_step panel_wake[] = { { 0x53, 1, 0, { 0x20 } } };
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
	.smem_subtype = 0x33,
	/* AUO H139BLN01, 400x400 AMOLED in command mode on one lane, TE
	 * into the MDP. */
	.panel = {
		.width = 400, .height = 400,
		.hz = 60, .te_gpio = 24, .round = true, .reset_gpio = 25,
		.wake = panel_wake, .wake_n = 1,
		.idle_on = idle_on, .idle_on_n = sizeof idle_on / sizeof idle_on[0],
		.idle_off = idle_off, .idle_off_n = sizeof idle_off / sizeof idle_off[0],
	},
	.pmic_mpm = 62,
	.key_reset_ms = 6720,
	/* Which of the three buttons is which is not known yet: RESIN and
	 * gpio 90 and 91 all act as the power key. */
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
	.motor = &motor_pm8226,
	.motor_pins = { .vib_mv = 2000 },
};
