/* Steps from accelerometer samples, for a watch whose sensor counts
 * none itself. The shape is that of Espruino's open pedometer
 * (libs/misc/stepcount.c): the magnitude at 12.5 Hz, its slow part
 * taken off, low-passed, and a step at each rise past a threshold
 * that comes at a walking pace. Counting starts after PEDOMETER_START
 * such steps, tuned on recorded wrist walks, and a walk ends after 1.5 s
 * without one. Samples go in at 25 Hz, in mg; the resampler brings
 * another rate to that. */
#ifndef PEDOMETER_H
#define PEDOMETER_H
#include <stdbool.h>
#include <stdint.h>

#define PEDOMETER_PERIOD_MS	40
#define PEDOMETER_START		5
#define PEDOMETER_THRESHOLD_MG	50
#define PEDOMETER_TAPS		5

struct pedometer {
	int32_t dc;
	int16_t hp[PEDOMETER_TAPS];
	int16_t half[3];
	bool have_half, primed, high, counting;
	uint8_t at, warm, pending;
	uint16_t since, interval;
	uint16_t threshold_mg;
	uint32_t steps, rejected;
};

void pedometer_init(struct pedometer *p);
/* The steps the sample added. */
unsigned pedometer_feed(struct pedometer *p, const int16_t v[3]);
/* No samples for ms: the filter starts over, and a walk may end. */
void pedometer_gap(struct pedometer *p, uint32_t ms);

/* Linear, from period_us to PEDOMETER_PERIOD_MS; the first sample
 * after a reset comes out as it is. */
struct resample {
	int16_t prev[3];
	uint32_t phase_us;
	bool have;
};

#define RESAMPLE_MAX	3

void resample_reset(struct resample *r);
/* The samples out, at most RESAMPLE_MAX for a period up to 80 ms. */
unsigned resample_feed(struct resample *r, const int16_t v[3], uint32_t period_us, int16_t out[RESAMPLE_MAX][3]);
#endif
