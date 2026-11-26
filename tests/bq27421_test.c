/* The bq27421's commands against a register file standing in for the chip. */
#include <stdio.h>
#include <string.h>
#include "soc/bq27421.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static uint8_t regs[0x62];
static uint16_t type = BQ27421_TYPE, status = 0x2080, last_sub;
static bool fail, cfg_stuck;
static unsigned unseal_keys, waited;
/* Data memory blocks 0 of the Registers and State classes. */
static uint8_t mem_regs[32], mem_state[32];

static uint8_t *block(void) { return regs[BQ27421_DATA_CLASS] == BQ27421_CLASS_REGISTERS ? mem_regs : mem_state; }

static uint8_t sum(const uint8_t *b)
{
	uint8_t s = 0;
	for (unsigned i = 0; i < 32; i++)
		s = (uint8_t)(s + b[i]);
	return (uint8_t)(255 - s);
}

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
	if (reg >= BQ27421_BLOCK_DATA && reg < BQ27421_BLOCK_SUM)
		memcpy(regs + BQ27421_BLOCK_DATA, block(), 32);
	if (reg == BQ27421_BLOCK_SUM)
		regs[reg] = sum(block());
	memcpy(buf, regs + reg, n);
	return 0;
}

static int wr(uint8_t reg, const uint8_t *buf, unsigned n)
{
	if (fail)
		return -1;
	if (reg == BQ27421_CONTROL && n == 2) {
		last_sub = (uint16_t)(buf[0] | buf[1] << 8);
		uint16_t flags = (uint16_t)(regs[BQ27421_FLAGS] | regs[BQ27421_FLAGS + 1] << 8);
		if (last_sub == BQ27421_UNSEAL_KEY && ++unseal_keys == 2)
			status &= (uint16_t)~BQ27421_STATUS_SS;
		if (last_sub == BQ27421_SEALED)
			status |= BQ27421_STATUS_SS;
		if (last_sub == BQ27421_SET_CFGUPDATE)
			flags |= BQ27421_FLAG_CFGUPMODE;
		if (last_sub == BQ27421_EXIT_CFGUPDATE && !cfg_stuck)
			flags &= (uint16_t)~BQ27421_FLAG_CFGUPMODE;
		put(BQ27421_FLAGS, flags);
		return 0;
	}
	if (n == 1 && reg >= BQ27421_BLOCK_DATA && reg < BQ27421_BLOCK_SUM)
		block()[reg - BQ27421_BLOCK_DATA] = buf[0];
	/* The gauge takes the block only with the checksum that fits it. */
	if (n == 1 && reg == BQ27421_BLOCK_SUM && buf[0] != sum(block()))
		return -9;
	if (n == 1)
		regs[reg] = buf[0];
	return 0;
}

static void wait(unsigned ms) { waited += ms; }

static const struct bq27421_io io = { rd, wr, wait };

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
	fail = false;

	/* Sealing and the vendor's power off data: OpConfig bits 0 and 2
	 * and UpdateStatus bit 7 cleared, the rest of the blocks kept. */
	status = 0x0080;
	CHECK(bq27421_seal(&io) == 0 && (status & BQ27421_STATUS_SS));
	put(BQ27421_FLAGS, BQ27421_FLAG_BAT_DET);
	mem_regs[0] = 0x25;
	mem_regs[1] = 0xf8;
	mem_state[2] = 0x83;
	mem_state[3] = 0x11;
	unseal_keys = 0;
	CHECK(bq27421_power_off_data(&io) == 0);
	CHECK(mem_regs[0] == 0x20 && mem_regs[1] == 0xf8 && mem_state[2] == 0x03 && mem_state[3] == 0x11);
	CHECK(!(regs[BQ27421_FLAGS] & BQ27421_FLAG_CFGUPMODE) && !(status & BQ27421_STATUS_SS));
	/* Still sealed after the keys: nothing written. */
	status |= BQ27421_STATUS_SS;
	unseal_keys = 3;
	mem_regs[0] = 0x25;
	CHECK(bq27421_power_off_data(&io) == -5 && mem_regs[0] == 0x25);
	/* A gauge that stays in config update mode is given up on. */
	unseal_keys = 0;
	cfg_stuck = true;
	waited = 0;
	CHECK(bq27421_power_off_data(&io) == -4 && waited >= 100 * 100);
	cfg_stuck = false;

	if (failures)
		return 1;
	printf("bq27421_test: ok\n");
	return 0;
}
