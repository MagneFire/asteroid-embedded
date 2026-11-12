#ifndef STEPS_H
#define STEPS_H
#include <stdint.h>
#include <stdbool.h>

#define STEPS_DAYS	30

void steps_init(void);
/* True when the steps today or the day changed. */
bool steps_poll(void);
void steps_save(void);
uint32_t steps_today(void);
/* Steps since the first boot, for counting from a moment on. */
uint32_t steps_total(void);
/* The steps `ago` days before today, 0 for today. */
uint32_t steps_day(unsigned ago);
/* Until just after the next midnight, at most an hour, so a clock set
 * meanwhile is seen. */
uint32_t steps_next_ms(void);
#endif
