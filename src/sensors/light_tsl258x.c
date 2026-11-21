/* beluga's light sensor, a TSL258x on I2C with the touch controller and
 * powered all the time. The vendor's firmware runs it at 111x and 100 ms
 * while the screen is on and clears its power bit otherwise; here it is
 * powered only while kept (light_keep), so only while awake.
 *
 * Kept on, the chip converts one integration after another by itself
 * and light_finish reads the last one: two short transfers and no
 * waiting, as the bus is the touch controller's too. The ADC goes on at
 * the first start or finish POWER_MS after the power bit, and a reading
 * counts once a whole integration has run since. A reading that needs
 * the other gain gives none and the next one uses it.
 *
 * The vendor's firmware leaves the chip alone unless its ID register
 * says TSL258x; so does this driver, and the ID goes to the log. */
#include "asteroid.h"
#include "sensors/tsl258x.h"

#define BUS		(board->light_pins.i2c.bus)
#define ADDR		(board->light_pins.i2c.addr)
#define BUS_HZ		400000

#define CMD_SELECT	0x80
#define CMD_AUTO_INC	0x20
#define CMD_SPECIAL	0x60
#define SPECIAL_INT_CLEAR 0x01

#define REG_CONTROL	0x00
#define REG_ITIME	0x01
#define REG_INTERRUPT	0x02
#define REG_ANALOG	0x07
#define REG_ID		0x12
#define REG_DATA0LOW	0x14

#define CONTROL_POWER	0x01
#define CONTROL_ADC_EN	0x02
#define CONTROL_VALID	0x10
#define ITIME_100MS	(256 - TSL258X_CYCLES)

static const uint8_t gain_codes[] = { 0x00, 0x03 };

/* The vendor's firmware waits 2 ms after the power bit. */
#define POWER_MS	3
#define INTEGRATION_MS	100
#define SLACK_MS	10

static bool ready, kept, adc;
static enum tsl258x_gain gain = TSL258X_GAIN_111X;
static uint32_t powered, valid_at;

static int wr(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { CMD_SELECT | reg, val };
	return i2c_xfer(BUS, ADDR, b, 2, NULL, 0);
}

static int rd(uint8_t reg, uint8_t *v, unsigned n)
{
	uint8_t cmd = CMD_SELECT | reg;
	return i2c_xfer(BUS, ADDR, &cmd, 1, v, n);
}

/* The interrupt line is not used: off, and nothing left pending. */
static bool tsl258x_init(void)
{
	uint8_t id = 0, clear = CMD_SELECT | CMD_SPECIAL | SPECIAL_INT_CLEAR;
	int r = i2c_init(BUS, board->light_pins.i2c.sda, board->light_pins.i2c.scl, board->light_pins.i2c.func, BUS_HZ);
	if (!r)
		r = rd(REG_ID, &id, 1);
	if (r) {
		log_printf("light: tsl258x not found (%d)\n", r);
		return false;
	}
	if (!tsl258x_id_ok(id)) {
		log_printf("light: id %02x is no tsl258x, left alone\n", id);
		return false;
	}
	r = wr(REG_CONTROL, 0);
	if (!r)
		r = wr(REG_INTERRUPT, 0);
	if (!r)
		r = i2c_xfer(BUS, ADDR, &clear, 1, NULL, 0);
	ready = !r;
	log_printf("light: tsl258x, id %02x%s\n", id, ready ? "" : ", no answer to its setup");
	return ready;
}

static bool adc_on(uint32_t now)
{
	if (adc)
		return true;
	if (now - powered < POWER_MS || wr(REG_CONTROL, CONTROL_POWER | CONTROL_ADC_EN))
		return false;
	adc = true;
	valid_at = now + INTEGRATION_MS + SLACK_MS;
	return true;
}

static void tsl258x_start(void)
{
	if (kept)
		adc_on(time_ms());
}

static int32_t tsl258x_finish(void)
{
	uint32_t now = time_ms();
	uint8_t status = 0, b[2];
	if (!kept || !adc_on(now) || (int32_t)(now - valid_at) < 0)
		return -1;
	if (rd(REG_CONTROL, &status, 1) || !(status & CONTROL_VALID) || rd(REG_DATA0LOW | CMD_AUTO_INC, b, sizeof b))
		return -1;
	uint16_t c0 = (uint16_t)(b[0] | b[1] << 8);
	enum tsl258x_gain next = tsl258x_next_gain(gain, c0);
	if (next == gain)
		return tsl258x_lux(gain, c0);
	/* The integration under way mixes both gains. */
	if (!wr(REG_ANALOG, gain_codes[next])) {
		gain = next;
		valid_at = now + 2 * INTEGRATION_MS + SLACK_MS;
	}
	return -1;
}

static void tsl258x_keep(bool on)
{
	if (!ready || on == kept)
		return;
	adc = false;
	if (on) {
		powered = time_ms();
		kept = !wr(REG_CONTROL, CONTROL_POWER) && !wr(REG_ITIME, ITIME_100MS) && !wr(REG_ANALOG, gain_codes[gain]);
		if (kept)
			return;
	}
	kept = false;
	wr(REG_CONTROL, 0);
}

const struct light_driver light_tsl258x = {
	.init = tsl258x_init,
	.start = tsl258x_start,
	.finish = tsl258x_finish,
	.keep = tsl258x_keep,
};
