/* A magnetometer's hard-iron calibration, learned from its own samples
 * as the watch moves, and the orientation from gravity and the
 * calibrated field: no hardware involved. Fields are in µT, Android's
 * axes; accuracies are Android's SENSOR_STATUS_* (0-3). */
#ifndef COMPASS_H
#define COMPASS_H
#include <stdbool.h>
#include <stdint.h>

#define COMPASS_BINS	26

struct compass_cal {
	float offset[3], radius;
	int32_t status;
};

struct compass {
	struct compass_cal cal;
	/* The latest sample in each of the directions around the centre. */
	float bin[COMPASS_BINS][3];
	uint32_t filled;
	float lo[3], hi[3];
	unsigned updates, off_field;
	bool seen, suspect, changed;
};

/* saved is a calibration from before, or NULL. */
void compass_init(struct compass *c, const struct compass_cal *saved);
/* A raw sample: out is it calibrated, the result its accuracy. */
int compass_feed(struct compass *c, const float raw[3], float out[3]);
unsigned compass_bins(const struct compass *c);
/* Azimuth (0-360), pitch (±180) and roll (±90) in degrees, from the
 * accelerometer (any unit) and the calibrated field; false if the
 * field is along gravity or either is 0. */
bool compass_orientation(const float g[3], const float m[3], float out[3]);
#endif
