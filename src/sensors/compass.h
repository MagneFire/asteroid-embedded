/* A magnetometer's calibration as the vendor's compass library does it,
 * no hardware involved: a fixed correction for the watch, an average
 * of the last COMPASS_AVG samples and a hard-iron offset learned from
 * the samples as the watch turns. The offset is a sphere fitted
 * through samples in COMPASS_BINS directions around it, trusted as far
 * as the directions cover, the samples lie on it and the gyroscope's
 * turns explain how they changed. A field that jumps by as much as a
 * magnet's sets the accuracy to 0 and starts the samples over, the
 * trusted offset kept. Fields are in µT, Android's axes; accuracies are
 * Android's SENSOR_STATUS_* (0-3). */
#ifndef COMPASS_H
#define COMPASS_H
#include <stdbool.h>
#include <stdint.h>

#define COMPASS_BINS	26
#define COMPASS_AVG	8

struct compass_cal {
	float offset[3], radius;
	int32_t level;
};

/* Sums over the recent samples, older ones fading, from which how well
 * an offset explains the field's changes by the turns is told. */
struct compass_turns {
	float uu, wu[3], ww[6], wm[3], mm, angle, n;
};

struct compass {
	struct compass_cal cal;
	int status;
	float avg[COMPASS_AVG][3], gyro[COMPASS_AVG][3], dt[COMPASS_AVG];
	unsigned avg_n, avg_at;
	uint32_t fed;
	float out[3], ref[3], last[3];
	bool have_ref, have_last, gyro_seen;
	float bin[COMPASS_BINS][3];
	uint32_t filled;
	float lo[3], hi[3];
	bool have_range;
	unsigned quiet, updates, off_field;
	struct compass_turns turns;
	uint32_t jumps, turn_resets;
};

void compass_init(struct compass *c);
/* After a gap in the samples: the calibration is kept. */
void compass_restart(struct compass *c);
/* The fixed correction alone. */
void compass_correct(const float raw[3], float out[3]);
/* A raw sample, with the gyroscope's rate in rad/s (or NULL) and the
 * time since the last one: out is the field, the result its accuracy. */
int compass_feed(struct compass *c, const float raw[3], const float gyro[3], float dt, float out[3]);
/* A sample out of the magnetometer's range: a magnet, as for a jump.
 * The result is the accuracy. */
int compass_overflow(struct compass *c);
unsigned compass_bins(const struct compass *c);
#endif
