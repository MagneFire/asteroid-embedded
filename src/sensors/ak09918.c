/* The AK09918 runs in its continuous mode at the rate asked for and
 * each read takes ST1, the axes and ST2 in one transfer, which also
 * lets the chip go on to the next measurement. The bus is the touch
 * controller's, so nothing here waits on it. Its address is fixed and
 * the bus is the one of the chips put to sleep at boot, which also
 * powers it down: with no measurement for a while, the mode is set
 * again. */
#include "asteroid.h"
#include "sensors/ak09918.h"

#define MAG		(board->mag_i2c.addr ? &board->mag_i2c : &board->chips_off.i2c)
#define BUS		(MAG->bus)
#define ADDR		0x0c
/* narwhal's bus runs at 100 kHz in the vendor's tree. */
#define BUS_HZ		(board->mag_i2c.addr ? 100000 : 400000)

#define WIA1		0x00
#define ST1		0x10
#define CNTL2		0x31
#define COMPANY		0x48
#define DEVICE		0x0c
#define ST1_DRDY	0x01
#define ST2_HOFL	0x08
#define LOST_PERIODS	4

static bool checked, present, unsure;
static uint8_t mode;
static uint32_t period, last_t;
static uint32_t reads, overflows, errors, restarts, misses;

static int wr(uint8_t reg, uint8_t v)
{
	uint8_t w[2] = { reg, v };
	int r = i2c_xfer(BUS, ADDR, w, 2, NULL, 0);
	errors += r != 0;
	return r;
}

/* A mode is only taken from power-down, 100 µs after it. A write that
 * timed out may still have reached the chip, so its mode is not known
 * until one is acknowledged. */
static bool set_mode(uint8_t m)
{
	unsure = true;
	if (wr(CNTL2, 0))
		return false;
	if (m) {
		delay_us(100);
		if (wr(CNTL2, m))
			return false;
	}
	unsure = false;
	last_t = time_ms();
	return true;
}

static bool probe(void)
{
	const struct board_i2c *b = MAG;
	uint8_t reg = WIA1, id[2] = { 0 };
	int r = i2c_init(b->bus, b->sda, b->scl, b->func, BUS_HZ);
	if (!r)
		r = i2c_xfer(BUS, ADDR, &reg, 1, id, 2);
	if (r) {
		if (!misses++)
			log_printf("compass: no answer (%d)\n", r);
		return false;
	}
	checked = true;
	present = id[0] == COMPANY && id[1] == DEVICE;
	log_printf("compass: %s (id %02x %02x)\n", present ? "AK09918" : "no AK09918", id[0], id[1]);
	return present;
}

bool ak09918_on(uint32_t period_ms)
{
	uint8_t m = ak09918_mode(period_ms);
	if (!checked && (!m || !probe()))
		return !m || checked;
	if (!present)
		return true;
	period = period_ms;
	if (m != mode || unsure) {
		if (!set_mode(m))
			return false;
		mode = m;
	}
	return true;
}

void ak09918_lost(void)
{
	checked = unsure = false;
	mode = 0;
}

int ak09918_read(int16_t v[3])
{
	uint8_t reg = ST1, b[9];
	if (!mode)
		return AK09918_NONE;
	uint32_t now = time_ms();
	if (i2c_xfer(BUS, ADDR, &reg, 1, b, sizeof b)) {
		errors++;
		return AK09918_NONE;
	}
	if (!(b[0] & ST1_DRDY)) {
		if (now - last_t >= LOST_PERIODS * period + 100 && set_mode(mode))
			restarts++;
		return AK09918_NONE;
	}
	last_t = now;
	reads++;
	if (b[8] & ST2_HOFL) {
		overflows++;
		return AK09918_OVERFLOW;
	}
	for (int i = 0; i < 3; i++)
		v[i] = (int16_t)(b[1 + 2 * i] | b[2 + 2 * i] << 8);
	return AK09918_OK;
}

void ak09918_status(void (*say)(const char *fmt, ...))
{
	if (checked || misses)
		say("compass: %s, mode %02x, %lu reads, %lu overflows, %lu errors, %lu restarts, %lu unanswered\n",
		    !checked ? "not answering" : present ? "AK09918" : "absent", mode, reads, overflows, errors, restarts,
		    misses);
}
