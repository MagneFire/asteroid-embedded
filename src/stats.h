#ifndef STATS_H
#define STATS_H
#include <stdint.h>
#include <stdbool.h>

void stats_init(uint32_t start_ms);
void stats_poll(uint32_t now, bool awake);
uint32_t stats_next_ms(void);
#endif
