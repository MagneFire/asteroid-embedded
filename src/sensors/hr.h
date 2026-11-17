/* Heart rate from a PPG signal at 25 Hz: band-pass, one peak per pulse,
 * the median of the last intervals between peaks. */
#ifndef HR_H
#define HR_H
#include <stdbool.h>
#include <stdint.h>

#define HR_HZ		25
/* A sample this far out is clipped: about 0.95 of the AFE4900's
 * +/-2^21. */
#define HR_CLIP		1990000

void hr_reset(void);
/* True when the sample gave a new reading. */
bool hr_add(int32_t x);
/* The last reading in bpm, 0 for none; quality 2 when every interval
 * agreed, 1 when most did. */
unsigned hr_bpm(uint8_t *quality);
#endif
