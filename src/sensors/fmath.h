/* The few float functions the sensor code needs, without a libm. */
#ifndef FMATH_H
#define FMATH_H
#include <stdbool.h>

#define FM_PI	3.14159265f

static inline float fm_sqrt(float v)
{
#if defined(__arm__) && defined(__ARM_FP)
	float r;
	__asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(v));
	return r;
#else
	return __builtin_sqrtf(v);
#endif
}

static inline float fm_abs(float v) { return v < 0 ? -v : v; }

/* Within 1e-5 rad. */
static inline float fm_atan2(float y, float x)
{
	float ax = fm_abs(x), ay = fm_abs(y);
	if (ax == 0 && ay == 0)
		return 0;
	bool swap = ay > ax;
	float t = swap ? ax / ay : ay / ax, t2 = t * t;
	float a = t * (0.9998660f + t2 * (-0.3302995f + t2 * (0.1801410f + t2 * (-0.0851330f + t2 * 0.0208351f))));
	if (swap)
		a = FM_PI / 2 - a;
	if (x < 0)
		a = FM_PI - a;
	return y < 0 ? -a : a;
}

static inline float fm_asin(float v)
{
	v = v > 1 ? 1 : v < -1 ? -1 : v;
	return fm_atan2(v, fm_sqrt(1 - v * v));
}
#endif
