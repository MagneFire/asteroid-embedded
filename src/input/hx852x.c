/* A report is four bytes per point (x high, x low, y high, y low; 0xffff
 * for none), one width byte per point padded to four, then the point
 * count (low nibble, 0xff for none), a mask of the fingers down (two
 * bytes, low first) and a byte that makes the sum of all 16 come to 0.
 * With no fingers, a first point at (0xfa5a, 0xfa5a) of width 0 is a
 * palm. */
#include <string.h>
#include "hx852x.h"

#define AREA		(HX852X_MAX_POINTS * 4)
#define COUNT		(AREA + 4)
#define MASK		(COUNT + 1)
#define PALM_AT		0xfa5a

void hx852x_parse(const uint8_t *buf, struct hx852x_report *r)
{
	memset(r, 0, sizeof *r);
	uint8_t sum = 0;
	for (unsigned i = 0; i < HX852X_REPORT_LEN; i++)
		sum = (uint8_t)(sum + buf[i]);
	if (sum) {
		r->bad = true;
		return;
	}
	unsigned count = buf[COUNT] == 0xff ? 0 : buf[COUNT] & 0x0f;
	uint16_t x0 = (uint16_t)(buf[0] << 8 | buf[1]), y0 = (uint16_t)(buf[2] << 8 | buf[3]);
	if (!count) {
		r->palm = x0 == PALM_AT && y0 == PALM_AT && buf[AREA] == 0;
		return;
	}
	unsigned mask = (unsigned)(buf[MASK + 1] << 8 | buf[MASK]);
	for (unsigned i = 0; i < HX852X_MAX_POINTS; i++) {
		if (!(mask & 1u << i))
			continue;
		const uint8_t *p = buf + 4 * i;
		r->p[r->n++] = (struct hx852x_point){ (uint8_t)i, (uint16_t)(p[0] << 8 | p[1]), (uint16_t)(p[2] << 8 | p[3]) };
	}
}

const struct hx852x_point *hx852x_follow(const struct hx852x_report *r, int id)
{
	for (unsigned i = 0; i < r->n; i++)
		if (id < 0 || r->p[i].id == id)
			return &r->p[i];
	return NULL;
}

bool hx852x_is_wake(const uint8_t *buf)
{
	return !memcmp(buf, "Wake", HX852X_WAKE_LEN);
}
