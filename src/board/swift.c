/* ASUS ZenWatch 3 (swift): APQ8009W with a PM8916, the production (PR)
 * board. Only the parts catfish's drivers already cover: no touch,
 * charger, gauge or sensors yet, and Bluetooth is the SoC's own WCNSS,
 * which nothing here starts. */
#include "asteroid.h"
#include "display/display.h"

/* Brightness control on, as the vendor's on-commands have it. */
static const struct dcs_step panel_wake[] = { { 0x53, 1, 0, { 0x20 } } };

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
	},
	.pmic_mpm = 62,
	.key_reset_ms = 6720,
	.usb_phy_init = { 0x73, 0x33, 0x07, 0x13 },
	.motor = &motor_pm8226,
	.motor_pins = { .vib_mv = 2000 },
};
