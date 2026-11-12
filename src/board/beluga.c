/* OPPO Watch (beluga), run by its Apollo3 Blue microcontroller with the
 * application processor off. Two sizes: the 46 mm watch with a 402x476
 * panel and the 41 mm one with a 320x360 panel, otherwise wired the
 * same but for one enable line. Pins are the Apollo3's pad numbers. */
#include "asteroid.h"
#include "display/display.h"

/* The panel's SPI master, IOM5: MOSI, SCK, MISO, and its chip select 3
 * on pad 45. */
static const struct board_mcu_pad pads[] = {
	{ 47, 5, PAD_STRONG },
	{ 48, 5, PAD_STRONG },
	{ 49, 5, PAD_INPUT },
	{ 45, 1, PAD_NCE(2) },
	{ 0, 0xff, 0 },
};

/* From reset to the picture on: the user command page, the partial area
 * over the visible part of the panel's memory, tearing on, brightness
 * control on, out of sleep. Windows and the pixel format are the
 * driver's. */
static const struct dcs_step panel_on_46[] = {
	{ 0xfe, 1, 0, { 0x00 } },
	{ 0x31, 4, 0, { 0x00, 0x28, 0x01, 0xb9 } },
	{ 0x30, 4, 0, { 0x00, 0x00, 0x01, 0xdb } },
	{ 0x12, 0, 0, { 0 } },
	{ 0x35, 1, 0, { 0x00 } },
	{ 0x53, 1, 0, { 0x20 } },
	{ 0x11, 0, 64, { 0 } },
	{ 0x29, 0, 0, { 0 } },
	{ 0xc4, 1, 0, { 0x80 } },
	{ 0x80, 1, 0, { 0x4f } },
};

static const struct dcs_step panel_on_41[] = {
	{ 0xfe, 1, 0, { 0x05 } },
	{ 0xa3, 1, 0, { 0x80 } },
	{ 0xfe, 1, 0, { 0x00 } },
	{ 0x31, 4, 0, { 0x00, 0x14, 0x01, 0x53 } },
	{ 0x30, 4, 0, { 0x00, 0x00, 0x01, 0x67 } },
	{ 0x12, 0, 0, { 0 } },
	{ 0x35, 1, 0, { 0x00 } },
	{ 0x53, 1, 0, { 0x20 } },
	{ 0x11, 0, 85, { 0 } },
	{ 0x29, 0, 0, { 0 } },
	{ 0xc4, 1, 0, { 0x80 } },
	{ 0x80, 1, 0, { 0x4f } },
};

/* RM69090 AMOLED on IOM5 at 48 MHz, mode 3: data/command on pad 46,
 * reset on pad 10, tearing on pad 11. The touch controller and the
 * haptics driver share IOM0's I2C (SCL pad 5, SDA pad 6). The battery is
 * on ADC channel 7 (pad 35) behind a divider that makes the 2.0 V
 * reference 7.2 V. Keys on pads 38 (power) and 15. The LSM6DSO
 * accelerometer is alone on IOM3's I2C (SCL pad 42, SDA pad 43), its
 * INT1 on pad 13. */
#define BELUGA_COMMON                                                                                                  \
	.mcu_pads = pads, .heart_rate = false,                                                                         \
	.battery_driver = &battery_adc, .battery_adc = { .pad = 35, .func = 0, .channel = 7, .mv_full = 7200 },        \
	.key_gpio = { 38, 15 },                                                                                        \
	.touch = &touch_tma525,                                                                                        \
	.hub = &hub_lsm6dso, .hub_pins = { .i2c = { .bus = 3, .sda = 43, .scl = 42, .func = 4, .addr = 0x6a }, .irq = 13 }, \
	.motor = &motor_aw8624, .motor_pins = { .i2c = { .bus = 0, .sda = 6, .scl = 5, .func = 0, .addr = 0x5a }, .enable = 26 }

const struct board board_beluga = {
	.name = "beluga",
	.compatible = "oppo,beluga",
	BELUGA_COMMON,
	.panel = {
		.width = 402, .height = 476, .hz = 60, .te_gpio = 11, .reset_gpio = 10,
		.on = panel_on_46, .on_n = ARRAY_SIZE(panel_on_46),
		.spi_bus = 5, .spi_cs = 3, .spi_mode = 3, .spi_hz = 48000000, .dc_gpio = 46, .enable_gpio = { 7, 12 },
		.col_offset = 40,
	},
	.touch_pins = { .i2c = { .bus = 0, .sda = 6, .scl = 5, .func = 0, .addr = 0x24 }, .irq = 23, .reset = 22, .power_gpio = 36 },
};

/* The 41 mm watch: pad 12 switches the touch controller instead of the
 * panel. */
const struct board board_beluga_41 = {
	.name = "beluga-41",
	.compatible = "oppo,beluga",
	BELUGA_COMMON,
	.panel = {
		.width = 320, .height = 360, .hz = 60, .te_gpio = 11, .reset_gpio = 10,
		.on = panel_on_41, .on_n = ARRAY_SIZE(panel_on_41),
		.spi_bus = 5, .spi_cs = 3, .spi_mode = 3, .spi_hz = 48000000, .dc_gpio = 46, .enable_gpio = { 7, 0 },
		.col_offset = 20,
	},
	.touch_pins = { .i2c = { .bus = 0, .sda = 6, .scl = 5, .func = 0, .addr = 0x24 }, .irq = 23, .reset = 22, .power_gpio = 12 },
};
