#ifndef STEPS_H
#define STEPS_H
#include <stdint.h>

void steps_init(void);
void steps_poll(void);
void steps_save(void);
uint32_t steps_today(void);
#endif
