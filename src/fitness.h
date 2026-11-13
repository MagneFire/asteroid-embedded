#ifndef FITNESS_H
#define FITNESS_H
#include <stdint.h>

uint32_t fitness_distance_m(uint32_t steps, unsigned height_cm);
uint32_t fitness_kcal(uint32_t distance_m, unsigned weight_kg);
#endif
