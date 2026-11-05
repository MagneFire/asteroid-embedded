/* LM3535 LED driver for the LCD's backlight, on I2C with a hardware
 * enable line. The light sensor stays off and the LEDs on bank A are
 * set directly, in the order the vendor's driver does: enable line,
 * bank enable, trim on the two early chip revisions, then the code.
 * Off is code 0, the banks off and the line low. */
#include "asteroid.h"
#include "display/lm3535.h"

#define QUP		(board->backlight_pins.i2c.qup)
#define ADDR		(board->backlight_pins.i2c.addr)
#define GPIO_ENABLE	(board->backlight_pins.enable)
#define BUS_HZ		200000

#define REG_ENABLE	0x10
#define REG_CONFIG	0x20
#define REG_OPTIONS	0x30
#define REG_ALS_CTRL	0x50
#define REG_ALS_RES	0x51
#define REG_BRIGHT_A	0xa0
#define REG_TRIM	0xd0
#define BANKS_ON	0x07
#define TRIM_VALUE	0x68
#define REVISION_2	0x02
#define REVISION_3	0x00

static uint8_t revision;
static bool lit;

static int wr(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { reg, val };
	return i2c_qup_xfer(QUP, ADDR, b, 2, NULL, 0);
}

static void banks_on(void)
{
	tlmm_set(GPIO_ENABLE, 1);
	delay_ms(1);
	wr(REG_ENABLE, BANKS_ON);
	if (revision == REVISION_2 || revision == REVISION_3)
		wr(REG_TRIM, TRIM_VALUE);
	lit = true;
}

static bool lm3535_init(void)
{
	tlmm_set(GPIO_ENABLE, 1);
	tlmm_config(GPIO_ENABLE, 0, TLMM_OUTPUT, TLMM_PULL_NONE, 8);
	delay_ms(1);
	uint8_t reg = REG_ALS_CTRL;
	int r = i2c_qup_init(QUP, board->backlight_pins.i2c.sda, board->backlight_pins.i2c.scl,
			     board->backlight_pins.i2c.func, BUS_HZ);
	if (!r)
		r = i2c_qup_xfer(QUP, ADDR, &reg, 1, NULL, 0);
	delay_ms(1);
	if (!r)
		r = i2c_qup_xfer(QUP, ADDR, NULL, 0, &revision, 1);
	if (r) {
		log_printf("backlight: lm3535 not found (%d)\n", r);
		return false;
	}
	wr(REG_CONFIG, 0);
	wr(REG_ALS_RES, 0);
	wr(REG_OPTIONS, 0);
	banks_on();
	log_printf("backlight: lm3535 revision %02x\n", revision);
	return true;
}

static void lm3535_set(uint8_t level)
{
	uint8_t code = lm3535_code(level);
	if (code && !lit)
		banks_on();
	wr(REG_BRIGHT_A, code);
	if (!code && lit) {
		wr(REG_ENABLE, 0);
		tlmm_set(GPIO_ENABLE, 0);
		lit = false;
	}
}

const struct backlight_driver backlight_lm3535 = { .init = lm3535_init, .set = lm3535_set };
