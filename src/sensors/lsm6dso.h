/* The LSM6DSO's gyroscope on beluga, for the AP only. */
#ifndef LSM6DSO_H
#define LSM6DSO_H
#include <stdbool.h>
#include <stdint.h>

/* At the 2000 dps range. */
#define LSM6DSO_MDPS_PER_COUNT	70

/* The output data rate for a sample every period_ms, 0 for off: codes
 * 1-3 are 12.5, 26 and 52 Hz, the slowest that keeps up. */
static inline uint8_t lsm6dso_gyro_odr(uint32_t period_ms)
{
	return !period_ms ? 0 : period_ms >= 80 ? 1 : period_ms >= 39 ? 2 : 3;
}
/* Off at 0; false if the chip did not take it, to be asked again. */
bool lsm6dso_gyro_on(uint32_t period_ms);
/* The output registers in counts, chip axes; false while off or
 * settling. */
bool lsm6dso_gyro(int16_t v[3]);
#endif
