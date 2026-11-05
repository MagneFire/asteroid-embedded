/* The MAX17050 register decoding. */
#include <stdio.h>
#include "soc/max17050.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

int main(void)
{
	CHECK(max17050_percent(0x0000) == 0);
	CHECK(max17050_percent(0x32ff) == 50);
	CHECK(max17050_percent(0x6400) == 100);
	/* A gauge that overshoots stays at 100. */
	CHECK(max17050_percent(0x6600) == 100);

	CHECK(max17050_mv(0) == 0);
	CHECK(max17050_mv(51200) == 4000);
	CHECK(max17050_mv(0xffff) == 5119);

	/* 10 mOhm: 156.25 uA a count, the gauge counts charging as positive. */
	CHECK(max17050_ua(0, 10000) == 0);
	CHECK(max17050_ua(1000, 10000) == -156250);
	CHECK(max17050_ua((uint16_t)-1000, 10000) == 156250);
	CHECK(max17050_ua(0x7fff, 10000) == -5119843);
	CHECK(max17050_ua(0x8000, 10000) == 5120000);

	/* Whole gauge degrees land on the vendor's table. */
	CHECK(max17050_decidegrees(25 * 256) == 256);
	CHECK(max17050_decidegrees(0) == 22);
	CHECK(max17050_decidegrees((uint16_t)(-10 * 256)) == -101);
	/* In between it interpolates. */
	CHECK(max17050_decidegrees(25 * 256 + 128) == 261);
	/* Past either end it holds the end. */
	CHECK(max17050_decidegrees((uint16_t)(-40 * 256)) == -302);
	CHECK(max17050_decidegrees(100 * 256) == 800);

	CHECK(max17050_lost_model(0x0002));
	CHECK(!max17050_lost_model(0x0008));
	CHECK(max17050_no_battery(0x0008));
	CHECK(!max17050_no_battery(0x0002));

	puts(failures ? "gauge_test: FAILED" : "gauge_test: ok");
	return failures != 0;
}
