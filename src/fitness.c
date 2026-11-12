/* What steps come to: a stride of 41.5 % of the height, a common estimate
 * for walking, and 0.5 kcal per 1000 steps for each kg of weight. */
#include "fitness.h"

uint32_t fitness_distance_m(uint32_t steps, unsigned height_cm)
{
	return (uint32_t)((uint64_t)steps * height_cm * 415 / 100000);
}

uint32_t fitness_kcal(uint32_t steps, unsigned weight_kg)
{
	return (uint32_t)((uint64_t)steps * weight_kg / 2000);
}
