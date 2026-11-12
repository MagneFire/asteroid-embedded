/* FocalTech FT3267: what the 21 bytes read from register 0 mean, no
 * hardware involved. */
#ifndef FTS_H
#define FTS_H
#include <stdbool.h>
#include <stdint.h>

#define FTS_REPORT_LEN	21
#define FTS_MAX_POINTS	2
/* The controller reports in the panel's 400 x 400. */
#define FTS_RANGE	400

struct fts_point {
	uint8_t id;
	bool down;
	uint16_t x, y;
};

struct fts_report {
	unsigned n;
	/* The first point sits at (2000, 2000): a palm on the panel. */
	bool palm;
	struct fts_point p[FTS_MAX_POINTS];
};

void fts_parse(const uint8_t *buf, struct fts_report *r);
/* The finger with this id while it stays down, or with -1 the first one down. */
const struct fts_point *fts_follow(const struct fts_report *r, int id);
#endif
