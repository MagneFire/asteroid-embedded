/* The bq27421's commands against a register file standing in for the chip. */
#include <stdio.h>
#include <string.h>
#include "soc/bq27421.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static uint8_t regs[0x40];
static uint16_t type = BQ27421_TYPE, status = 0x2080, last_sub;
static bool fail;

static void put(uint8_t reg, uint16_t v)
{
	regs[reg] = (uint8_t)v;
	regs[reg + 1] = (uint8_t)(v >> 8);
}

/* Control answers the last subcommand written to it. */
static int rd(uint8_t reg, uint8_t *buf, unsigned n)
{
	if (fail)
		return -1;
	if (reg == BQ27421_CONTROL)
		put(BQ27421_CONTROL, last_sub == BQ27421_DEVICE_TYPE ? type : last_sub == 0 ? status : 0xffff);
	memcpy(buf, regs + reg, n);
	return 0;
}

static int wr(uint8_t reg, const uint8_t *buf, unsigned n)
{
	if (fail)
		return -1;
	if (reg == BQ27421_CONTROL && n == 2)
		last_sub = (uint16_t)(buf[0] | buf[1] << 8);
	return 0;
}

static const struct bq27421_io io = { rd, wr };

int main(void)
{
	uint16_t t = 0, st = 0;
	CHECK(bq27421_probe(&io, &t, &st) == 0 && t == BQ27421_TYPE && st == 0x2080);
	type = 0x0426;
	CHECK(bq27421_probe(&io, &t, &st) == -2 && t == 0x0426);
	type = BQ27421_TYPE;
	fail = true;
	CHECK(bq27421_probe(&io, &t, &st) == -1);
	fail = false;

	/* 25.0 C, 4123 mV, charging at 150 mA, 87 %. */
	put(BQ27421_TEMP, 2982);
	put(BQ27421_VOLTAGE, 4123);
	put(BQ27421_FLAGS, BQ27421_FLAG_BAT_DET);
	put(BQ27421_AVG_CURRENT, 150);
	put(BQ27421_SOC, 87);
	struct bq27421_reading r;
	CHECK(bq27421_read(&io, &r) == 0);
	CHECK(r.mv == 4123 && r.soc == 87 && r.avg_ma == 150 && r.decidegrees == 250 && r.flags == BQ27421_FLAG_BAT_DET);
	/* Discharging is negative, below 0 C too. */
	put(BQ27421_AVG_CURRENT, (uint16_t)-42);
	put(BQ27421_TEMP, 2682);
	CHECK(bq27421_read(&io, &r) == 0 && r.avg_ma == -42 && r.decidegrees == -50);
	put(BQ27421_SOC, 250);
	CHECK(bq27421_read(&io, &r) == 0 && r.soc == 100);
	fail = true;
	CHECK(bq27421_read(&io, &r) == -1);

	if (failures)
		return 1;
	printf("bq27421_test: ok\n");
	return 0;
}
