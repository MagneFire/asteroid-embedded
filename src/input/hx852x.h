/* Himax HX852xES: what the 16 bytes read with command 0x86 mean, no
 * hardware involved. */
#ifndef HX852X_H
#define HX852X_H
#include <stdbool.h>
#include <stdint.h>

#define HX852X_REPORT_LEN	16
#define HX852X_MAX_POINTS	2
#define HX852X_WAKE_LEN		4
/* The controller reports in the panel's 400 x 400. */
#define HX852X_RANGE		400

struct hx852x_point {
	uint8_t id;
	uint16_t x, y;
};

struct hx852x_report {
	/* The bytes do not add up to 0: a report to drop. */
	bool bad;
	bool palm;
	unsigned n;
	struct hx852x_point p[HX852X_MAX_POINTS];
};

void hx852x_parse(const uint8_t *buf, struct hx852x_report *r);
/* The finger with this id while it stays down, or with -1 the first one. */
const struct hx852x_point *hx852x_follow(const struct hx852x_report *r, int id);
/* The idle mode's wake packet. */
bool hx852x_is_wake(const uint8_t *buf);
#endif
