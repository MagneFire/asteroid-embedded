/* What steps come to: a stride of 41.5 % of the height, a common estimate
 * for walking, and 0.5 kcal per km for each kg of weight, the energy of
 * walking on top of resting. */
#include "fitness.h"

uint32_t fitness_distance_m(uint32_t steps, unsigned height_cm)
{
	return (uint32_t)((uint64_t)steps * height_cm * 415 / 100000);
}

uint32_t fitness_kcal(uint32_t distance_m, unsigned weight_kg)
{
	return (uint32_t)((uint64_t)distance_m * weight_kg / 2000);
}
