/* The AK09918 magnetometer on beluga's IOM0, with the touch controller,
 * for the AP only. */
#ifndef AK09918_H
#define AK09918_H
#include <stdbool.h>
#include <stdint.h>

#define AK09918_UT_PER_COUNT	0.15f

/* The continuous mode for a sample every period_ms, 0 for power-down:
 * 10, 20, 50 or 100 Hz, the slowest with a new measurement at each
 * sample despite the main loop's jitter. */
static inline uint8_t ak09918_mode(uint32_t period_ms)
{
	return !period_ms ? 0 : period_ms >= 125 ? 0x02 : period_ms >= 63 ? 0x04 : period_ms >= 25 ? 0x06 : 0x08;
}
/* Off at 0; false while the chip has not taken it, to be asked again.
 * The first one that is not 0 asks for its ID, again until it answers. */
bool ak09918_on(uint32_t period_ms);
/* The chip was put to sleep behind the driver: it starts over. */
void ak09918_lost(void);
enum { AK09918_NONE = -1, AK09918_OK, AK09918_OVERFLOW };
/* The last measurement in counts, chip axes: AK09918_NONE while off or
 * without a new one, AK09918_OVERFLOW if the field was out of range. */
int ak09918_read(int16_t v[3]);
void ak09918_status(void (*say)(const char *fmt, ...));
#endif
