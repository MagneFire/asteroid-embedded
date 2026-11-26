#include "bq27421.h"

static int word(const struct bq27421_io *io, uint8_t reg, uint16_t *v)
{
	uint8_t b[2];
	int r = io->read(reg, b, 2);
	if (!r)
		*v = (uint16_t)(b[0] | b[1] << 8);
	return r;
}

int bq27421_control(const struct bq27421_io *io, uint16_t sub, uint16_t *out)
{
	uint8_t b[2] = { (uint8_t)sub, (uint8_t)(sub >> 8) };
	int r = io->write(BQ27421_CONTROL, b, 2);
	return r ? r : word(io, BQ27421_CONTROL, out);
}

int bq27421_probe(const struct bq27421_io *io, uint16_t *type, uint16_t *status)
{
	int r = bq27421_control(io, BQ27421_DEVICE_TYPE, type);
	if (!r)
		r = bq27421_control(io, 0x0000, status);
	if (r)
		return r;
	return *type == BQ27421_TYPE ? 0 : -2;
}

int bq27421_read(const struct bq27421_io *io, struct bq27421_reading *r)
{
	uint16_t t, mv, flags, cur, soc;
	int e;
	if ((e = word(io, BQ27421_TEMP, &t)) || (e = word(io, BQ27421_VOLTAGE, &mv)) ||
	    (e = word(io, BQ27421_FLAGS, &flags)) || (e = word(io, BQ27421_AVG_CURRENT, &cur)) ||
	    (e = word(io, BQ27421_SOC, &soc)))
		return e;
	/* Temperature in 0.1 K. */
	*r = (struct bq27421_reading){ mv, flags, (uint8_t)(soc > 100 ? 100 : soc), (int16_t)cur, (int16_t)(t - 2732) };
	return 0;
}

static int sub(const struct bq27421_io *io, uint16_t cmd)
{
	uint8_t b[2] = { (uint8_t)cmd, (uint8_t)(cmd >> 8) };
	return io->write(BQ27421_CONTROL, b, 2);
}

static int byte_wr(const struct bq27421_io *io, uint8_t reg, uint8_t v) { return io->write(reg, &v, 1); }

int bq27421_seal(const struct bq27421_io *io) { return sub(io, BQ27421_SEALED); }

#define ENTER_TRIES	10
#define EXIT_TRIES	100
#define CFG_WAIT_MS	100
#define UNSEAL_WAIT_MS	100
#define BLOCK_WAIT_MS	10

static int cfgupdate(const struct bq27421_io *io, bool on)
{
	for (unsigned i = 0;; i++) {
		uint16_t flags;
		int r = word(io, BQ27421_FLAGS, &flags);
		if (r)
			return r;
		if (!!(flags & BQ27421_FLAG_CFGUPMODE) == on)
			return 0;
		if (i == (on ? ENTER_TRIES : EXIT_TRIES))
			return -4;
		if ((r = sub(io, on ? BQ27421_SET_CFGUPDATE : BQ27421_EXIT_CFGUPDATE)))
			return r;
		io->wait_ms(CFG_WAIT_MS);
	}
}

/* One byte of a data memory block changed under mask, with the block's
 * checksum kept right. */
static int mem_byte(const struct bq27421_io *io, uint8_t cls, uint8_t off, uint8_t mask, uint8_t v)
{
	uint8_t sum, old;
	int r;
	if ((r = byte_wr(io, BQ27421_DATA_CLASS, cls)) || (r = byte_wr(io, BQ27421_DATA_BLOCK, 0)))
		return r;
	io->wait_ms(BLOCK_WAIT_MS);
	if ((r = io->read(BQ27421_BLOCK_SUM, &sum, 1)) || (r = io->read((uint8_t)(BQ27421_BLOCK_DATA + off), &old, 1)))
		return r;
	uint8_t now = (uint8_t)((old & ~mask) | (v & mask));
	if ((r = byte_wr(io, (uint8_t)(BQ27421_BLOCK_DATA + off), now)))
		return r;
	return byte_wr(io, BQ27421_BLOCK_SUM, (uint8_t)(sum + old - now));
}

int bq27421_power_off_data(const struct bq27421_io *io)
{
	uint16_t status;
	int r;
	if ((r = sub(io, BQ27421_UNSEAL_KEY)) || (r = sub(io, BQ27421_UNSEAL_KEY)))
		return r;
	io->wait_ms(UNSEAL_WAIT_MS);
	if ((r = bq27421_control(io, 0x0000, &status)))
		return r;
	if (status & BQ27421_STATUS_SS)
		return -5;
	if ((r = cfgupdate(io, true)) || (r = byte_wr(io, BQ27421_BLOCK_CTRL, 0)) ||
	    (r = mem_byte(io, BQ27421_CLASS_REGISTERS, 0, 0x05, 0)) || (r = mem_byte(io, BQ27421_CLASS_STATE, 2, 0x80, 0)))
		return r;
	return cfgupdate(io, false);
}
