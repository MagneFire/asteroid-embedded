#ifndef HRLOG_H
#define HRLOG_H
#include <stdint.h>
#include <stdbool.h>

#define HRLOG_SAMPLES	144
#define HRLOG_TIMEOUT_S	120

void hrlog_init(void);
/* Every interval_min minutes, 0 for never: whether the sensor has to be
 * on for a reading. steady_bpm is the rate once it can be kept, else 0. */
bool hrlog_poll(uint32_t rtc, unsigned interval_min, uint32_t steady_bpm);
uint32_t hrlog_next_ms(uint32_t rtc, unsigned interval_min);
/* The readings from `since` on, oldest first: their times (RTC) and rates. */
unsigned hrlog_get(uint32_t since, uint32_t *t, uint8_t *bpm, unsigned max);
/* Readings taken elsewhere, oldest first, saved once. */
void hrlog_add(const uint32_t *t, const uint8_t *bpm, unsigned n);
#endif
