/* The lux to brightness curves, their hysteresis and the median. */
#include <stdio.h>
#include "sensors/auto_brightness.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static int at(int setting, int32_t lux)
{
	struct auto_brightness a = { 0 };
	auto_brightness_add(&a, lux);
	return auto_brightness_percent(&a, setting);
}

int main(void)
{
	struct auto_brightness a = { 0 };
	/* The setting itself until there is a reading. */
	CHECK(auto_brightness_percent(&a, 40) == 40);

	/* The ends of the curves the settings pick: 1 %, 10 %, 50 %, 100 %. */
	CHECK(at(1, 0) == 1);
	CHECK(at(10, 0) == 1);
	/* The sensor never gives under 2 lux, and from there on the lowest
	 * setting stays clear of the levels that turn the backlight off. */
	CHECK(at(10, 2) == 6);
	for (int st = 10; st <= 100; st += 10)
		CHECK(at(st, 2) >= 4);
	CHECK(at(50, 0) == 5);
	CHECK(at(100, 0) == 25);
	CHECK(at(1, 2000) == 100);
	CHECK(at(100, 1000) == 100);
	CHECK(at(100, 900) == 98);
	CHECK(at(100, 989168) == 100);
	/* A step ends at its limit. */
	CHECK(at(100, 1) == 31);
	CHECK(at(50, 350) == 55);
	CHECK(at(50, 349) == 50);

	/* Every curve goes up with the lux, and every setting's starts at
	 * no less than the one below it. */
	for (int s = 1; s <= 100; s++)
		for (int32_t l = 1; l < 2100; l++) {
			CHECK(at(s, l) >= at(s, l - 1));
			CHECK(at(s, l) <= 100);
			if (s > 1)
				CHECK(at(s, l) >= at(s - 1, l));
		}

	/* The median of the last five, the first reading at once. */
	struct auto_brightness m = { 0 };
	auto_brightness_add(&m, 100);
	CHECK(m.lux == 100);
	auto_brightness_add(&m, 300);
	auto_brightness_add(&m, 50);
	CHECK(m.lux == 100);
	auto_brightness_add(&m, 2000);
	auto_brightness_add(&m, 2000);
	CHECK(m.lux == 300);
	auto_brightness_add(&m, 2000);
	CHECK(m.lux == 2000);
	/* The lux stays over a reset; the next reading replaces it. */
	auto_brightness_reset(&m);
	CHECK(m.lux == 2000);
	auto_brightness_add(&m, 7);
	CHECK(m.lux == 7);

	/* Falling, the brightness holds a tenth of a step past the limit;
	 * rising it holds up to the top of its step. Setting 100 %: 75 %
	 * is 109 to 195 lux. */
	struct auto_brightness h = { 0 };
#define FEED(l) do { auto_brightness_reset(&h); auto_brightness_add(&h, (l)); } while (0)
	FEED(150);
	CHECK(auto_brightness_percent(&h, 100) == 75);
	FEED(105);
	CHECK(auto_brightness_percent(&h, 100) == 75);
	FEED(104);
	CHECK(auto_brightness_percent(&h, 100) == 69);
	FEED(150);
	CHECK(auto_brightness_percent(&h, 100) == 75);
	FEED(195);
	CHECK(auto_brightness_percent(&h, 100) == 75);
	FEED(196);
	CHECK(auto_brightness_percent(&h, 100) == 80);
	/* A new setting takes its own curve straight away. */
	CHECK(auto_brightness_percent(&h, 10) == 26);

	puts(failures ? "auto_brightness_test: FAILED" : "auto_brightness_test: ok");
	return failures != 0;
}
