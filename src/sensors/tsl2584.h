/* The TSL2584 light sensor's counts as lux and lux as the always-on
 * face's backlight code, no hardware involved. */
#ifndef TSL2584_H
#define TSL2584_H
#include <stdint.h>

/* 8x gain for dim light, 1x for bright. */
enum tsl2584_mode { TSL2584_LOW_LUX, TSL2584_HIGH_LUX };

#define TSL2584_OVERRANGE	(-1)

/* The lux for two channel counts, at least 2. coeff is lux1's channel 0
 * and 1 and lux2's channel 0 and 1. TSL2584_OVERRANGE at low lux means
 * the counts make no sense at this gain. */
int32_t tsl2584_lux(enum tsl2584_mode mode, uint16_t c0, uint16_t c1, const uint16_t coeff[4]);
/* The mode for the next reading after one of `lux`. */
enum tsl2584_mode tsl2584_next_mode(enum tsl2584_mode mode, int32_t lux);
/* The LM3535 code for the always-on face. */
uint8_t tsl2584_backlight_code(uint32_t lux);
#endif
