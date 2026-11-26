#include <string.h>
#include "sensors/pedometer.h"

/* In samples at 12.5 Hz: 1.5 s ends a walk, steps at most 3 a second. */
#define END_SAMPLES	19
#define MIN_INTERVAL	4
/* The slow part follows the magnitude by 1/8 a sample, kept in 1/16 mg. */
#define DC_SHIFT	3
#define DC_FRAC		4
#define PERIOD_US	(PEDOMETER_PERIOD_MS * 1000)

static const int16_t taps[PEDOMETER_TAPS] = { 1, 4, 6, 4, 1 };

void pedometer_init(struct pedometer *p)
{
	memset(p, 0, sizeof *p);
	p->since = UINT16_MAX;
	p->threshold_mg = PEDOMETER_THRESHOLD_MG;
}

static uint32_t isqrt(uint32_t v)
{
	uint32_t r = 0, bit = 1u << 30;
	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

static void end_walk(struct pedometer *p)
{
	p->rejected += p->pending;
	p->pending = 0;
	p->interval = 0;
	p->counting = false;
}

static void tick(struct pedometer *p, uint32_t n)
{
	p->since = p->since + n > UINT16_MAX ? UINT16_MAX : (uint16_t)(p->since + n);
	if (p->since > END_SAMPLES && (p->pending || p->counting))
		end_walk(p);
}

/* Until the walk counts, each step comes within a half of the time the
 * one before it took; one that does not starts the walk over. */
static unsigned step(struct pedometer *p)
{
	uint16_t i = p->since;
	if (i < MIN_INTERVAL)
		return 0;
	p->since = 0;
	if (p->counting) {
		p->steps++;
		return 1;
	}
	if (p->pending > 1 && (3 * i < 2 * p->interval || 2 * i > 3 * p->interval)) {
		p->rejected += p->pending;
		p->pending = 0;
	}
	p->interval = p->pending ? i : 0;
	if (++p->pending < PEDOMETER_START)
		return 0;
	unsigned n = p->pending;
	p->steps += n;
	p->pending = 0;
	p->counting = true;
	return n;
}

static unsigned core(struct pedometer *p, const int32_t a[3])
{
	int32_t mag = (int32_t)isqrt((uint32_t)(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]));
	if (!p->primed) {
		p->dc = mag << DC_FRAC;
		p->primed = true;
		p->warm = 0;
		p->high = false;
	}
	p->dc += ((mag << DC_FRAC) - p->dc) / (1 << DC_SHIFT);
	int32_t hp = mag - p->dc / (1 << DC_FRAC);
	p->hp[p->at] = (int16_t)(hp > INT16_MAX ? INT16_MAX : hp < INT16_MIN ? INT16_MIN : hp);
	p->at = (uint8_t)((p->at + 1) % PEDOMETER_TAPS);
	tick(p, 1);
	if (p->warm < PEDOMETER_TAPS) {
		p->warm++;
		return 0;
	}
	int32_t f = 0;
	for (int k = 0; k < PEDOMETER_TAPS; k++)
		f += taps[k] * p->hp[(p->at + k) % PEDOMETER_TAPS];
	f /= 16;
	if (p->high) {
		if (f < 0)
			p->high = false;
		return 0;
	}
	if (f <= p->threshold_mg)
		return 0;
	p->high = true;
	return step(p);
}

unsigned pedometer_feed(struct pedometer *p, const int16_t v[3])
{
	if (!p->have_half) {
		memcpy(p->half, v, sizeof p->half);
		p->have_half = true;
		return 0;
	}
	p->have_half = false;
	int32_t a[3];
	for (int i = 0; i < 3; i++)
		a[i] = ((int32_t)p->half[i] + v[i]) / 2;
	return core(p, a);
}

void pedometer_gap(struct pedometer *p, uint32_t ms)
{
	p->primed = p->have_half = false;
	tick(p, ms / (2 * PEDOMETER_PERIOD_MS));
}

void resample_reset(struct resample *r) { r->have = false; }

unsigned resample_feed(struct resample *r, const int16_t v[3], uint32_t period_us, int16_t out[RESAMPLE_MAX][3])
{
	unsigned n = 0;
	if (!r->have || !period_us) {
		memcpy(out[0], v, sizeof out[0]);
		memcpy(r->prev, v, sizeof r->prev);
		r->phase_us = PERIOD_US;
		r->have = true;
		return 1;
	}
	for (; r->phase_us <= period_us && n < RESAMPLE_MAX; n++, r->phase_us += PERIOD_US)
		for (int i = 0; i < 3; i++)
			out[n][i] = (int16_t)(r->prev[i] + (int64_t)(v[i] - r->prev[i]) * r->phase_us / period_us);
	r->phase_us = r->phase_us > period_us ? r->phase_us - period_us : PERIOD_US;
	memcpy(r->prev, v, sizeof r->prev);
	return n;
}
