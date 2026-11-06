/* The MAX17050 register decoding. */
#include <stdio.h>
#include "soc/max17050.h"
#include "soc/charge_policy.h"

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

	/* Too hot stops, and needs a degree to start again. */
	struct charge_policy p = { 0 };
	struct charge_input in = { .now_ms = 100000, .valid = true, .current_valid = true, .temp = 300, .percent = 50, .mv = 3800, .charge_ma = 100 };
	CHECK(!charge_policy_step(&p, &in));
	in.temp = 530;
	CHECK(charge_policy_step(&p, &in));
	in.temp = 525;
	CHECK(charge_policy_step(&p, &in));
	in.temp = 519;
	CHECK(!charge_policy_step(&p, &in));

	/* Warm: 10 minutes on, 5 off, down to 42 C, across the timer's wrap. */
	p = (struct charge_policy){ 0 };
	in.temp = 450;
	in.now_ms = 0xfffff000u;
	CHECK(!charge_policy_step(&p, &in));
	in.now_ms += 599000;
	CHECK(!charge_policy_step(&p, &in));
	in.now_ms += 1000;
	CHECK(charge_policy_step(&p, &in));
	in.now_ms += 299000;
	CHECK(charge_policy_step(&p, &in));
	in.now_ms += 1000;
	CHECK(!charge_policy_step(&p, &in));
	in.now_ms += 600000;
	in.temp = 421;
	CHECK(charge_policy_step(&p, &in));
	in.temp = 419;
	CHECK(!charge_policy_step(&p, &in));

	/* Nothing known, no charging. */
	in.valid = false;
	CHECK(charge_policy_step(&p, &in));
	CHECK(!charge_temp_plausible(-200) && !charge_temp_plausible(800) && charge_temp_plausible(250));

	/* The rate: reduced over 4.1 V, back at 3.9 V. */
	p = (struct charge_policy){ 0 };
	in = (struct charge_input){ .now_ms = 100000, .valid = true, .current_valid = true, .temp = 300, .percent = 50, .mv = 4100, .charge_ma = 100 };
	charge_policy_step(&p, &in);
	CHECK(!p.reduced);
	in.mv = 4101;
	charge_policy_step(&p, &in);
	CHECK(p.reduced);
	in.mv = 3901;
	charge_policy_step(&p, &in);
	CHECK(p.reduced);
	in.mv = 3900;
	charge_policy_step(&p, &in);
	CHECK(!p.reduced);

	/* Full by low charge current, only that direction, and again from 98 %. */
	p = (struct charge_policy){ 0 };
	in = (struct charge_input){ .now_ms = 100000, .valid = true, .current_valid = true, .temp = 300, .percent = 99, .mv = 4300, .charge_ma = 10 };
	CHECK(!charge_policy_step(&p, &in));
	in.percent = 100;
	in.charge_ma = -10;
	for (int i = 0; i < 6; i++)
		CHECK(!charge_policy_step(&p, &in));
	in.charge_ma = 10;
	in.current_valid = false;
	CHECK(!charge_policy_step(&p, &in));
	in.current_valid = true;
	for (int i = 0; i < 4; i++)
		CHECK(!charge_policy_step(&p, &in));
	CHECK(charge_policy_step(&p, &in));
	in.percent = 99;
	CHECK(charge_policy_step(&p, &in));
	in.percent = 98;
	CHECK(!charge_policy_step(&p, &in));

	/* Already full at the first reading, long after the timer started. */
	p = (struct charge_policy){ 0 };
	in = (struct charge_input){ .now_ms = 4000000, .valid = true, .current_valid = true, .temp = 300, .percent = 100, .mv = 4300, .charge_ma = 200 };
	CHECK(!charge_policy_step(&p, &in));

	/* Full by time, counted from reaching 100 %. */
	p = (struct charge_policy){ 0 };
	in = (struct charge_input){ .now_ms = 100000, .valid = true, .current_valid = true, .temp = 300, .percent = 90, .mv = 4300, .charge_ma = 200 };
	CHECK(!charge_policy_step(&p, &in));
	in.now_ms += 3000000;
	in.percent = 99;
	CHECK(!charge_policy_step(&p, &in));
	in.percent = 100;
	CHECK(!charge_policy_step(&p, &in));
	in.now_ms += 1800000;
	CHECK(charge_policy_step(&p, &in));

	puts(failures ? "gauge_test: FAILED" : "gauge_test: ok");
	return failures != 0;
}
