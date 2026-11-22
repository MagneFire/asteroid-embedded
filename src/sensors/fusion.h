/* The watch's orientation as the vendor's compass library keeps it, no
 * hardware involved: the gyroscope's turns, levelled toward gravity and
 * turned toward the field's north while the field can be trusted, so
 * the heading carries on through a field that cannot. Android's axes;
 * the world's x is east, y north and z up. */
#ifndef FUSION_H
#define FUSION_H
#include <stdbool.h>

struct fusion {
	/* From the watch's axes to the world's. */
	float q[4];
	float bias[3], gravity, strength, dip;
	unsigned still;
	int level;
	bool started;
};

void fusion_init(struct fusion *f);
/* The accelerometer (any unit), the gyroscope in rad/s or NULL, the
 * calibrated field in µT or NULL with its accuracy (0-3), and the time
 * since the last update in s. */
void fusion_update(struct fusion *f, const float a[3], const float w[3], const float m[3], int accuracy, float dt);
/* Azimuth of y (0-360), pitch (±180) and roll (±90) in degrees; false
 * until the first update with a field. */
bool fusion_angles(const struct fusion *f, float out[3]);
#endif
