/* The AL3320B driver against a fake chip. */
#include <stdio.h>
#include <string.h>
#include "sensors/light_al3320.c"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const struct board test_board = {
	.light_pins = { .i2c = { .bus = 1, .sda = 6, .scl = 7, .func = 3, .addr = 0x1c } },
};
const struct board *board = &test_board;

/* The chip's registers and the clock. The count is per range. */
static uint8_t regs[0x40];
static uint16_t counts[4];
static bool absent;
static uint32_t now, enabled_at;

uint32_t time_ms(void) { return now; }
void delay_ms(uint32_t ms) { now += ms; }
void log_printf(const char *fmt, ...) {}
int i2c_init(unsigned bus, unsigned sda, unsigned scl, unsigned func, uint32_t bus_hz) { return 0; }
int i2c_xfer(unsigned bus, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	CHECK(bus == 1 && addr == 0x1c && wn);
	if (absent)
		return -2;
	uint8_t reg = w[0];
	if (wn == 2) {
		if (reg == REG_CONFIG && (w[1] & CONFIG_ENABLE) && !(regs[REG_CONFIG] & CONFIG_ENABLE))
			enabled_at = now;
		regs[reg] = w[1];
		return 0;
	}
	if (reg == REG_DATA) {
		/* A conversion takes 100 ms; before the first the data is 0. */
		CHECK(regs[REG_CONFIG] & CONFIG_ENABLE);
		uint16_t c = now - enabled_at >= CONVERSION_MS ? counts[regs[REG_RANGE] >> RANGE_SHIFT & 3] : 0;
		regs[REG_DATA] = (uint8_t)c;
		regs[REG_DATA + 1] = (uint8_t)(c >> 8);
	}
	for (unsigned i = 0; i < rn; i++)
		r[i] = regs[reg + i];
	return 0;
}

int main(void)
{
	absent = true;
	CHECK(!al3320_init());
	absent = false;
	CHECK(al3320_init());
	CHECK(regs[REG_MEAN_TIME] == MEAN_TIME && regs[REG_CONFIG] == 0);

	/* A room: 10000 counts in the fine range are 320 lux, off again after. */
	counts[RANGE_FINE] = 10000;
	al3320_start();
	CHECK(regs[REG_CONFIG] & CONFIG_ENABLE);
	CHECK(al3320_finish() == 320);
	CHECK(regs[REG_CONFIG] == 0);
	CHECK(al3320_finish() == -1);

	/* Daylight: the fine range is full, so it is read again coarse at once. */
	counts[RANGE_FINE] = 65535;
	counts[RANGE_COARSE] = 20000;
	al3320_start();
	CHECK(al3320_finish() == 10240);
	CHECK(!fine);

	/* Kept on, a range switch gives no reading, the next one does. */
	counts[RANGE_COARSE] = 1000;
	counts[RANGE_FINE] = 16000;
	al3320_keep(true);
	CHECK(regs[REG_CONFIG] & CONFIG_ENABLE);
	al3320_start();
	CHECK(al3320_finish() == -1);
	CHECK(fine);
	al3320_start();
	CHECK(al3320_finish() == 512);
	al3320_keep(false);
	CHECK(regs[REG_CONFIG] == 0);

	/* In the dark the data stays 0: a single reading still gives 0, also
	 * after switching down from the coarse range. */
	counts[RANGE_FINE] = 0;
	counts[RANGE_COARSE] = 0;
	al3320_start();
	CHECK(al3320_finish() == 0);
	fine = false;
	al3320_start();
	CHECK(al3320_finish() == 0 && fine);
	/* Kept on, the first 0 after enabling waits for the next read. */
	al3320_keep(true);
	al3320_start();
	CHECK(al3320_finish() == -1);
	al3320_start();
	CHECK(al3320_finish() == 0);
	al3320_keep(false);

	if (failures)
		printf("%d failures\n", failures);
	else
		printf("al3320: all passed\n");
	return failures != 0;
}
