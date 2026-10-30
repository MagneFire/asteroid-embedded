#ifndef WEATHER_H
#define WEATHER_H
#include <stdint.h>

#define WEATHER_DAYS 5

struct weather {
	char city[64];
	uint32_t time;
	uint16_t days;
	uint16_t id[WEATHER_DAYS], min[WEATHER_DAYS], max[WEATHER_DAYS];
};

void weather_write(uint16_t chr, const uint8_t *p, unsigned n);
const struct weather *weather_get(void);
uint32_t weather_serial(void);
void weather_load(void);
void weather_poll(void);
#endif
