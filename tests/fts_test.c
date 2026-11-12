/* FT3267 touch report parsing. */
#include <stdio.h>
#include <string.h>
#include "input/fts.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* Event and id go in the top bits of the first and third byte of a point. */
static void point(uint8_t *buf, unsigned i, unsigned event, unsigned id, unsigned x, unsigned y)
{
	uint8_t *p = buf + 3 + i * 6;
	p[0] = (uint8_t)(event << 6 | x >> 8);
	p[1] = (uint8_t)x;
	p[2] = (uint8_t)(id << 4 | y >> 8);
	p[3] = (uint8_t)y;
	p[4] = 0x20;
	p[5] = 0x40;
}

int main(void)
{
	struct fts_report r;
	uint8_t buf[FTS_REPORT_LEN];

	/* No fingers: the unused points carry id 15. */
	memset(buf, 0, sizeof buf);
	for (unsigned i = 0; i < FTS_MAX_POINTS; i++)
		point(buf, i, 3, 15, 0, 0);
	fts_parse(buf, &r);
	CHECK(r.n == 0 && !r.palm && !fts_follow(&r, -1));

	/* One finger, put down; x and y use all 12 bits. */
	buf[2] = 1;
	point(buf, 0, 0, 0, 0x123, 0x1ab);
	fts_parse(buf, &r);
	CHECK(r.n == 1 && r.p[0].id == 0 && r.p[0].down && r.p[0].x == 0x123 && r.p[0].y == 0x1ab);
	CHECK(fts_follow(&r, -1) == &r.p[0] && fts_follow(&r, 0) == &r.p[0] && !fts_follow(&r, 1));

	/* The same finger in contact, moved to the far corner. */
	point(buf, 0, 2, 0, 399, 399);
	fts_parse(buf, &r);
	CHECK(r.n == 1 && r.p[0].down && r.p[0].x == 399 && r.p[0].y == 399);

	/* Two fingers: the second one follows by its id. */
	buf[2] = 2;
	point(buf, 0, 2, 0, 100, 200);
	point(buf, 1, 0, 1, 300, 50);
	fts_parse(buf, &r);
	CHECK(r.n == 2 && r.p[1].id == 1 && r.p[1].x == 300 && r.p[1].y == 50);
	CHECK(fts_follow(&r, 1) == &r.p[1] && fts_follow(&r, -1) == &r.p[0]);

	/* The first lifts: it is listed but no longer down, so the other is
	 * the first one down and the lifted id is not followed. */
	point(buf, 0, 1, 0, 100, 200);
	fts_parse(buf, &r);
	CHECK(r.n == 2 && !r.p[0].down && r.p[1].down);
	CHECK(!fts_follow(&r, 0) && fts_follow(&r, -1) == &r.p[1]);

	/* Lift-off of the only finger, then an empty report. */
	buf[2] = 1;
	point(buf, 0, 1, 0, 100, 200);
	fts_parse(buf, &r);
	CHECK(r.n == 1 && !r.p[0].down && !fts_follow(&r, -1) && !fts_follow(&r, 0));
	buf[2] = 0;
	fts_parse(buf, &r);
	CHECK(r.n == 0);

	/* The count is clamped, an id of 15 ends the list. */
	memset(buf, 0, sizeof buf);
	buf[2] = 0x0f;
	point(buf, 0, 0, 3, 10, 20);
	point(buf, 1, 0, 4, 30, 40);
	fts_parse(buf, &r);
	CHECK(r.n == 2 && r.p[1].id == 4);
	buf[2] = 2;
	point(buf, 1, 0, 15, 30, 40);
	fts_parse(buf, &r);
	CHECK(r.n == 1);
	point(buf, 0, 0, 15, 10, 20);
	fts_parse(buf, &r);
	CHECK(r.n == 0);

	/* The high nibble of the count byte is not part of it. */
	buf[2] = 0xf1;
	point(buf, 0, 0, 2, 7, 9);
	fts_parse(buf, &r);
	CHECK(r.n == 1 && r.p[0].id == 2 && r.p[0].x == 7 && r.p[0].y == 9);

	/* A palm is a first point at exactly (2000, 2000). */
	buf[2] = 1;
	point(buf, 0, 0, 0, 2000, 2000);
	fts_parse(buf, &r);
	CHECK(r.n == 1 && r.palm);
	point(buf, 0, 0, 0, 2000, 1999);
	fts_parse(buf, &r);
	CHECK(!r.palm);
	buf[2] = 2;
	point(buf, 0, 0, 0, 100, 100);
	point(buf, 1, 0, 1, 2000, 2000);
	fts_parse(buf, &r);
	CHECK(!r.palm);

	if (failures)
		return 1;
	printf("fts_test: ok\n");
	return 0;
}
