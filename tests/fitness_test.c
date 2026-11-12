/* Distance and energy from steps on the host. */
#include <stdio.h>
#include "fitness.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

int main(void)
{
	CHECK(fitness_distance_m(0, 170) == 0);
	CHECK(fitness_distance_m(10000, 170) == 7055);
	CHECK(fitness_distance_m(100000000, 230) == 95450000);
	CHECK(fitness_kcal(8000, 70) == 280);
	CHECK(fitness_kcal(100000000, 200) == 10000000);

	printf(failures ? "fitness_test: %d failures\n" : "fitness_test: ok\n", failures);
	return failures != 0;
}
