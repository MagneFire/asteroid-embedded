/* TI's bq27421 fuel gauge, read through its standard commands, no
 * hardware involved. */
#ifndef BQ27421_H
#define BQ27421_H
#include <stdbool.h>
#include <stdint.h>

#define BQ27421_ADDR		0x55
#define BQ27421_CONTROL		0x00
#define BQ27421_TEMP		0x02
#define BQ27421_VOLTAGE		0x04
#define BQ27421_FLAGS		0x06
#define BQ27421_AVG_CURRENT	0x10
#define BQ27421_SOC		0x1c
#define BQ27421_DEVICE_TYPE	0x0001
#define BQ27421_TYPE		0x0421

#define BQ27421_FLAG_ITPOR	(1u << 5)
#define BQ27421_FLAG_CFGUPMODE	(1u << 4)
#define BQ27421_FLAG_BAT_DET	(1u << 3)
#define BQ27421_STATUS_SS	(1u << 13)
#define BQ27421_STATUS_INITCOMP	(1u << 7)

struct bq27421_io {
	int (*read)(uint8_t reg, uint8_t *buf, unsigned n);
	int (*write)(uint8_t reg, const uint8_t *buf, unsigned n);
};

/* Current positive while charging, as the gauge counts it. */
struct bq27421_reading {
	uint16_t mv, flags;
	uint8_t soc;
	int16_t avg_ma;
	int16_t decidegrees;
};

/* A Control subcommand's answer. */
int bq27421_control(const struct bq27421_io *io, uint16_t sub, uint16_t *out);
/* 0 for a bq27421, -2 for another device type. */
int bq27421_probe(const struct bq27421_io *io, uint16_t *type, uint16_t *status);
int bq27421_read(const struct bq27421_io *io, struct bq27421_reading *r);
#endif
