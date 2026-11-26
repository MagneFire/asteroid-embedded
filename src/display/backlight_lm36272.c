/* LM36272 backlight and LCD bias, on I2C with no enable line. Only the
 * backlight is driven here: the bias outputs stay as the bootloader set
 * them up for its splash. As the vendor does, levels of 0 to 255 go
 * through the board's map to the chip's 11-bit current code, the
 * overvoltage limit is set to 29 V before the LEDs go on, and the code
 * goes in before the LEDs are switched on. */
#include "asteroid.h"

#define BUS		(board->backlight_pins.i2c.bus)
#define ADDR		(board->backlight_pins.i2c.addr)
#define BUS_HZ		400000

#define REG_BL_CONFIG	0x02
#define REG_BRT_LSB	0x04
#define REG_BRT_MSB	0x05
#define REG_ENABLE	0x08
#define BL_OVP_29V	0x60
#define ENABLE_BL	0x13
#define CODE_MAX	0x7ff

static bool lit;

static int wr(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { reg, val };
	return i2c_xfer(BUS, ADDR, b, 2, NULL, 0);
}

static int rd(uint8_t reg, uint8_t *val) { return i2c_xfer(BUS, ADDR, &reg, 1, val, 1); }

static int write_code(uint16_t code)
{
	return wr(REG_BRT_LSB, code & 7) | wr(REG_BRT_MSB, (uint8_t)(code >> 3));
}

static void set_code(uint16_t code)
{
	if (!code) {
		write_code(0);
		wr(REG_ENABLE, 0);
		lit = false;
		return;
	}
	if (code > CODE_MAX)
		code = CODE_MAX;
	if (!lit) {
		wr(REG_BL_CONFIG, BL_OVP_29V);
		delay_ms(1);
	}
	if (!write_code(code) && !wr(REG_ENABLE, ENABLE_BL))
		lit = true;
}

static bool lm36272_init(void)
{
	uint8_t lsb = 0, msb = 0, en = 0;
	int r = i2c_init(BUS, board->backlight_pins.i2c.sda, board->backlight_pins.i2c.scl,
			 board->backlight_pins.i2c.func, BUS_HZ);
	if (!r)
		r = rd(REG_BRT_LSB, &lsb) | rd(REG_BRT_MSB, &msb) | rd(REG_ENABLE, &en);
	if (r) {
		log_printf("backlight: lm36272 not found (%d)\n", r);
		return false;
	}
	lit = en & ENABLE_BL;
	log_printf("backlight: lm36272, code %u, enable %02x from the bootloader\n", msb << 3 | (lsb & 7), en);
	return true;
}

static void lm36272_set(uint8_t level)
{
	set_code(level ? board->backlight_pins.map[level] : 0);
}

const struct backlight_driver backlight_lm36272 = { .init = lm36272_init, .set = lm36272_set };
