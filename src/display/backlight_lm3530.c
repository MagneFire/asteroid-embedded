/* LM3530 LED driver for the LCD's backlight, on I2C with a hardware
 * enable line, in the vendor's manual mode: brightness over I2C, linear
 * mapping, its maximum current setting, no ramps. The chip forgets its
 * registers with the line low, so lighting it up again writes them first.
 * Levels of 0 to 255 scale to the vendor's maximum code, 0x60 of 0x7f.
 * The brightness goes in before the LEDs are switched on, as it comes out
 * of reset at full scale. */
#include "asteroid.h"

#define BUS		(board->backlight_pins.i2c.bus)
#define ADDR		(board->backlight_pins.i2c.addr)
#define GPIO_ENABLE	(board->backlight_pins.enable)
#define BUS_HZ		200000

#define REG_GEN_CONFIG	0x10
#define REG_RAMP	0x30
#define REG_BRIGHT	0xa0
#define GEN_I2C		BIT(0)
#define GEN_LINEAR	BIT(1)
#define GEN_MAX_CURRENT(n)	((n) << 2)
#define MAX_CURRENT	2
#define CODE_MAX	0x60

static bool lit;

static int wr(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { reg, val };
	return i2c_xfer(BUS, ADDR, b, 2, NULL, 0);
}

static int light_up(uint8_t code)
{
	gpio_set(GPIO_ENABLE, 1);
	delay_ms(1);
	int r = wr(REG_RAMP, 0) | wr(REG_BRIGHT, code);
	if (!r)
		r = wr(REG_GEN_CONFIG, GEN_MAX_CURRENT(MAX_CURRENT) | GEN_LINEAR | GEN_I2C);
	if (r)
		gpio_set(GPIO_ENABLE, 0);
	lit = !r;
	return r;
}

static bool lm3530_init(void)
{
	gpio_set(GPIO_ENABLE, 1);
	gpio_config(GPIO_ENABLE, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	int r = i2c_init(BUS, board->backlight_pins.i2c.sda, board->backlight_pins.i2c.scl,
			 board->backlight_pins.i2c.func, BUS_HZ);
	uint8_t reg = REG_BRIGHT, code = 0;
	if (!r)
		r = i2c_xfer(BUS, ADDR, &reg, 1, &code, 1);
	if (!r)
		r = light_up(code > CODE_MAX ? CODE_MAX : code);
	if (r) {
		gpio_set(GPIO_ENABLE, 0);
		log_printf("backlight: lm3530 not found (%d)\n", r);
		return false;
	}
	log_printf("backlight: lm3530, code %02x from the bootloader\n", code);
	return true;
}

static void lm3530_set_code(uint8_t code)
{
	if (!code) {
		if (lit)
			wr(REG_BRIGHT, 0);
		gpio_set(GPIO_ENABLE, 0);
		lit = false;
		return;
	}
	if (code > CODE_MAX)
		code = CODE_MAX;
	if (!lit) {
		light_up(code);
		return;
	}
	wr(REG_BRIGHT, code);
}

static void lm3530_set(uint8_t level)
{
	uint8_t code = (uint8_t)((2 * level * CODE_MAX + 255) / 510);
	lm3530_set_code(level && !code ? 1 : code);
}

const struct backlight_driver backlight_lm3530 = { .init = lm3530_init, .set = lm3530_set, .set_code = lm3530_set_code };
