/* LG Watch W7 (narwhal): APQ8009 with a PM8916, the rev-10, rev-c and
 * rev-a boards. Two mechanical hands sit over the LCD. */
#include "asteroid.h"
#include "display/display.h"

/* The GPU microcode is on the vendor partition. */
static const char *const firmware_dirs[] = { "/firmware/", NULL };

const struct board board_narwhal = {
	.name = "narwhal",
	.compatible = "lge,narwhal",
	/* Sharp NT35350 360x360 command-mode LCD, TE into the MDP. */
	.panel = {
		.width = 360, .height = 360,
		.hz = 60, .te_gpio = 24, .round = true, .lcd = true, .reset_gpio = 25,
	},
	.firmware_dirs = firmware_dirs,
	.firmware_part = "vendor",
	.pmic_mpm = 62,
	.crash_to_fastboot = true,
	/* The top button is gpio 90 and the bottom one gpio 91. */
	.keys = { { .gpio = 90, .mpm = 30 }, { .gpio = 91, .mpm = 32 } },
	.usb_phy_init = { 0x44, 0x6f, 0x38, 0x33 },
	.motor = &motor_pm8226,
	.motor_pins = { .vib_mv = 2900 },
};
