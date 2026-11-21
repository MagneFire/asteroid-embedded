/* beluga's TSL258x light sensor: its channel 0 counts as lux and the
 * gain for the next reading, no hardware involved. */
#ifndef TSL258X_H
#define TSL258X_H
#include <stdbool.h>
#include <stdint.h>

/* 1x for bright light, 111x for the rest, the vendor's only gain. */
enum tsl258x_gain { TSL258X_GAIN_1X, TSL258X_GAIN_111X };

/* 2.7 ms cycles in an integration, the vendor's ITIME of 0xdb. */
#define TSL258X_CYCLES	37

/* Whether the ID register names a TSL258x. */
bool tsl258x_id_ok(uint8_t id);
/* The lux for a channel 0 count. */
int32_t tsl258x_lux(enum tsl258x_gain gain, uint16_t c0);
/* The gain for the next reading after a count of c0. */
enum tsl258x_gain tsl258x_next_gain(enum tsl258x_gain gain, uint16_t c0);
#endif
