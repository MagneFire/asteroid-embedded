/* Heart rate from a 25 Hz PPG signal. Two Butterworth sections make a
 * 0.6-3.5 Hz band-pass. A pulse is a hump of the filtered signal above
 * a threshold that follows the pulses' height, ended by the signal
 * going below zero; its time is the top of the hump, interpolated
 * between samples. Intervals outside 40-200 bpm are dropped; a reading
 * is the mean of the last intervals that lie near their median, once
 * enough of them do. A clipped sample starts it all over. */
#include "sensors/hr.h"

#define IVS		12
#define MIN_IV		(HR_HZ * 60.0f / 200)
#define MAX_IV		(HR_HZ * 60.0f / 40)
#define LOST		(3 * HR_HZ)
#define MIN_AMP		20.0f

/* 0.6 Hz high-pass and 3.5 Hz low-pass at 25 Hz; b1 and b2 follow from b0. */
#define HP_B0		0.89884553f
#define HP_A1		-1.78743252f
#define HP_A2		0.80794959f
#define LP_B0		0.11735104f
#define LP_A1		-0.82523238f
#define LP_A2		0.29463653f

static struct {
	uint32_t n;
	int32_t x1, x2;
	float h1, h2, y1, y2;
	float amp, top, top_t, last_t;
	bool hump, have_last;
	float iv[IVS];
	uint8_t ivn, ivi, quality;
	unsigned bpm;
} s;

void hr_reset(void)
{
	s = (typeof(s)){ 0 };
}

static float fabsf_(float v) { return v < 0 ? -v : v; }

static bool estimate(void)
{
	if (s.ivn < 5)
		return false;
	float v[IVS];
	unsigned n = s.ivn;
	for (unsigned i = 0; i < n; i++) {
		unsigned j = i;
		for (; j && v[j - 1] > s.iv[i]; j--)
			v[j] = v[j - 1];
		v[j] = s.iv[i];
	}
	float m = v[n / 2], sum = 0;
	unsigned k = 0;
	for (unsigned i = 0; i < n; i++)
		if (fabsf_(v[i] - m) < 0.12f * m) {
			sum += v[i];
			k++;
		}
	if (k < 5 || 4 * k < 3 * n)
		return false;
	s.bpm = (unsigned)(HR_HZ * 60.0f * (float)k / sum + 0.5f);
	s.quality = k == n ? 2 : 1;
	return true;
}

static bool pulse(float top, float t)
{
	s.amp = s.amp < top ? (s.amp + top) / 2 : (3 * s.amp + top) / 4;
	if (!s.have_last) {
		s.have_last = true;
		s.last_t = t;
		return false;
	}
	float iv = t - s.last_t;
	if (iv < MIN_IV)
		return false;
	s.last_t = t;
	if (iv > MAX_IV)
		return false;
	s.iv[s.ivi] = iv;
	s.ivi = (uint8_t)((s.ivi + 1) % IVS);
	if (s.ivn < IVS)
		s.ivn++;
	return estimate();
}

bool hr_add(int32_t x)
{
	if (x >= HR_CLIP || x <= -HR_CLIP) {
		hr_reset();
		return false;
	}
	if (!s.n)
		s.x1 = s.x2 = x;
	float t = (float)s.n++;
	float h = HP_B0 * (float)(x - 2 * s.x1 + s.x2) - HP_A1 * s.h1 - HP_A2 * s.h2;
	float y = LP_B0 * (h + 2 * s.h1 + s.h2) - LP_A1 * s.y1 - LP_A2 * s.y2;
	s.x2 = s.x1;
	s.x1 = x;
	s.h2 = s.h1;
	s.h1 = h;
	bool fresh = false;
	if (s.y1 > s.y2 && s.y1 >= y && s.y1 > 0.4f * s.amp && s.y1 > MIN_AMP) {
		if (!s.hump || s.y1 > s.top) {
			float d = s.y2 - 2 * s.y1 + y;
			s.hump = true;
			s.top = s.y1;
			s.top_t = t - 1 + (d < 0 ? 0.5f * (s.y2 - y) / d : 0);
		}
	} else if (s.hump && y < 0) {
		s.hump = false;
		fresh = pulse(s.top, s.top_t);
	}
	s.y2 = s.y1;
	s.y1 = y;
	s.amp *= 0.985f;
	if (s.have_last && t - s.last_t > LOST) {
		s.have_last = false;
		s.ivn = 0;
		s.bpm = 0;
	}
	return fresh;
}

unsigned hr_bpm(uint8_t *quality)
{
	if (quality)
		*quality = s.bpm ? s.quality : 0;
	return s.bpm;
}
