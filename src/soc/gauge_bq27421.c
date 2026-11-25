/* A bq27421 fuel gauge on I2C, only read: the bootloader writes the
 * cell's data into it. It counts on its own, so a reading is taken
 * whenever the charger asks and at most once a second. The gauge needs
 * a pause between commands. A gauge that did not answer is tried again
 * on the next ask, and after FAILS_STALE failed readings in a row the
 * last one is not given out. The vendor takes 0 % above 3.7 V as a fault
 * of the gauge and shows 50 % instead. */
#include "asteroid.h"
#include "soc/bq27421.h"

#define BUS		(board->gauge.i2c.bus)
#define ADDR		(board->gauge.i2c.addr)
#define BUS_HZ		100000
#define GAP_US		100
#define MIN_READ_MS	1000
#define FAILS_STALE	3
#define FAULT_MV	3700
#define FAULT_SOC	50

static struct hub_gauge last;
static bool ready, have, faulty;
static uint32_t read_t, errors, fails;
static uint16_t last_flags = 0xffff;

static int io_read(uint8_t reg, uint8_t *buf, unsigned n)
{
	delay_us(GAP_US);
	int r = i2c_xfer(BUS, ADDR, &reg, 1, buf, n);
	if (r)
		errors++;
	return r;
}

static int io_write(uint8_t reg, const uint8_t *buf, unsigned n)
{
	uint8_t b[3] = { reg };
	if (n > 2)
		return -3;
	for (unsigned i = 0; i < n; i++)
		b[1 + i] = buf[i];
	delay_us(GAP_US);
	int r = i2c_xfer(BUS, ADDR, b, n + 1, NULL, 0);
	if (r)
		errors++;
	return r;
}

static const struct bq27421_io io = { io_read, io_write };

static void take(void)
{
	struct bq27421_reading r;
	read_t = time_ms();
	if (bq27421_read(&io, &r)) {
		fails++;
		return;
	}
	fails = 0;
	if (r.flags != last_flags)
		log_printf("gauge: flags %04x%s, %u %% %u mV %d mA %d.%d C, %lu errors\n", r.flags,
			   r.flags & BQ27421_FLAG_ITPOR ? " (lost its cell data)" : "", r.soc, r.mv, r.avg_ma,
			   r.decidegrees / 10, (r.decidegrees < 0 ? -r.decidegrees : r.decidegrees) % 10, errors);
	last_flags = r.flags;
	bool fault = !r.soc && r.mv > FAULT_MV;
	if (fault != faulty)
		log_printf("gauge: %u %% at %u mV, %s\n", r.soc, r.mv, fault ? "taken as a fault" : "no longer a fault");
	faulty = fault;
	last = (struct hub_gauge){ r.mv, fault ? FAULT_SOC : r.soc, -r.avg_ma * 1000, r.decidegrees, read_t };
	have = true;
}

static bool start(void)
{
	uint16_t type = 0, status = 0;
	int r = bq27421_probe(&io, &type, &status);
	if (r) {
		log_printf("gauge: no bq27421 (%d, type %04x, %lu errors)\n", r, type, errors);
		return false;
	}
	ready = true;
	log_printf("gauge: bq27421, control status %04x%s%s\n", status, status & BQ27421_STATUS_SS ? ", sealed" : "",
		   status & BQ27421_STATUS_INITCOMP ? "" : ", not initialised");
	take();
	return true;
}

static bool bq27421_init(void)
{
	if (i2c_init(BUS, board->gauge.i2c.sda, board->gauge.i2c.scl, board->gauge.i2c.func, BUS_HZ))
		return false;
	return start();
}

static void bq27421_poll(bool now)
{
	if (!ready) {
		start();
		return;
	}
	if (!have || now || time_ms() - read_t >= MIN_READ_MS)
		take();
}

static bool bq27421_get(struct hub_gauge *g)
{
	if (!have || fails >= FAILS_STALE)
		return false;
	*g = last;
	return true;
}

const struct gauge_driver gauge_bq27421 = {
	.init = bq27421_init,
	.poll = bq27421_poll,
	.read = bq27421_get,
};
