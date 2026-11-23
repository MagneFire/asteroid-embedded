/* swift's light sensor, a Dyna Image AL3320B at 0x1c on its own I2C bus,
 * which the modem's sensor firmware drives under stock. Its rail is
 * always on. The chip converts continuously while enabled, so a reading
 * is the 16-bit count after at least one conversion since enabling it,
 * times the lux per count of its range. The interrupt line is not used.
 *
 * Two of its four ranges are used: 0.032 lux per count up to about
 * 2000 lux and 0.512 above. A count near the top of the fine range, or
 * near the bottom of the coarse one, gives no reading and switches; with
 * the chip kept on between readings the next one is in the new range,
 * otherwise it is read again at once. */
#include "asteroid.h"

#define BUS		(board->light_pins.i2c.bus)
#define ADDR		(board->light_pins.i2c.addr)
#define BUS_HZ		100000

#define REG_CONFIG	0x00
#define REG_WAIT	0x06
#define REG_RANGE	0x07
#define REG_MEAN_TIME	0x09
#define REG_DATA	0x22

#define CONFIG_ENABLE	0x01
#define MEAN_TIME	4
#define RANGE_COARSE	0
#define RANGE_FINE	2
#define RANGE_SHIFT	1

/* Lux per count, in thousandths. */
#define COARSE_MLUX	512
#define FINE_MLUX	32
#define FINE_TOP	60000
#define COARSE_BOTTOM	3000

#define CONVERSION_MS	100

static bool ready, running, kept, fine = true;
static uint32_t enabled_t;

static int wr(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { reg, val };
	return i2c_xfer(BUS, ADDR, b, 2, NULL, 0);
}

static int rd(uint8_t reg, uint8_t *v, unsigned n) { return i2c_xfer(BUS, ADDR, &reg, 1, v, n); }

static int enable(void)
{
	int r = wr(REG_RANGE, (fine ? RANGE_FINE : RANGE_COARSE) << RANGE_SHIFT);
	if (!r)
		r = wr(REG_CONFIG, CONFIG_ENABLE);
	enabled_t = time_ms();
	return r;
}

static void disable(void) { wr(REG_CONFIG, 0); }

/* -1 with no reading, after switching the range if the count asks for it. */
static int32_t measure(void)
{
	uint32_t gone = time_ms() - enabled_t;
	if (gone < CONVERSION_MS)
		delay_ms(CONVERSION_MS - gone);
	uint8_t b[2];
	if (rd(REG_DATA, b, 2))
		return -1;
	uint32_t count = (uint32_t)(b[0] | b[1] << 8);
	if (fine ? count >= FINE_TOP : count < COARSE_BOTTOM) {
		fine = !fine;
		if (wr(REG_RANGE, (fine ? RANGE_FINE : RANGE_COARSE) << RANGE_SHIFT))
			return -1;
		enabled_t = time_ms();
		return -1;
	}
	return (int32_t)(count * (fine ? FINE_MLUX : COARSE_MLUX) / 1000);
}

static bool al3320_init(void)
{
	int r = i2c_init(BUS, board->light_pins.i2c.sda, board->light_pins.i2c.scl, board->light_pins.i2c.func, BUS_HZ);
	uint8_t config = 0xff;
	if (!r)
		r = rd(REG_CONFIG, &config, 1);
	if (!r)
		r = wr(REG_MEAN_TIME, MEAN_TIME);
	if (!r)
		r = wr(REG_WAIT, 0);
	if (!r)
		r = wr(REG_CONFIG, 0);
	if (r) {
		log_printf("light: al3320 not found (%d)\n", r);
		return false;
	}
	ready = true;
	log_printf("light: al3320, config was %02x\n", config);
	return true;
}

static void al3320_start(void)
{
	if (!ready)
		return;
	running = kept || !enable();
	if (!running && !kept)
		disable();
}

static int32_t al3320_finish(void)
{
	if (!running)
		return -1;
	running = false;
	int32_t lux = measure();
	if (lux < 0 && !kept)
		lux = measure();
	if (!kept)
		disable();
	return lux;
}

static void al3320_keep(bool on)
{
	if (!ready || on == kept)
		return;
	running = false;
	kept = on && !enable();
	if (!kept)
		disable();
}

const struct light_driver light_al3320 = {
	.init = al3320_init,
	.start = al3320_start,
	.finish = al3320_finish,
	.keep = al3320_keep,
};
