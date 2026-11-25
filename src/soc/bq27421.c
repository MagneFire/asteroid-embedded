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
