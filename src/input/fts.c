/* A report is three bytes (the second is a gesture, unused) and then six
 * per point: event (bits 7-6, 0 down, 1 up, 2 contact) with x[11:8],
 * x[7:0], id (bits 7-4) with y[11:8], y[7:0], pressure and area. The
 * count is in the low nibble of byte 2; an id of 15 ends the list early. */
#include <string.h>
#include "fts.h"

#define HEAD_LEN	3
#define POINT_LEN	6
#define EVENT_UP	1
#define ID_END		0x0f
#define PALM_AT		2000

void fts_parse(const uint8_t *buf, struct fts_report *r)
{
	memset(r, 0, sizeof *r);
	unsigned n = buf[2] & 0x0f;
	if (n > FTS_MAX_POINTS)
		n = FTS_MAX_POINTS;
	for (unsigned i = 0; i < n; i++) {
		const uint8_t *p = buf + HEAD_LEN + i * POINT_LEN;
		unsigned id = p[2] >> 4, event = p[0] >> 6;
		if (id >= ID_END)
			break;
		r->p[r->n++] = (struct fts_point){ (uint8_t)id, event != EVENT_UP && event < 3,
						   (uint16_t)((p[0] & 0x0f) << 8 | p[1]), (uint16_t)((p[2] & 0x0f) << 8 | p[3]) };
	}
	r->palm = r->n && r->p[0].x == PALM_AT && r->p[0].y == PALM_AT;
}

const struct fts_point *fts_follow(const struct fts_report *r, int id)
{
	for (unsigned i = 0; i < r->n; i++)
		if (r->p[i].down && (id < 0 || r->p[i].id == id))
			return &r->p[i];
	return NULL;
}
