/* The sleep and wake decisions and nightstand timing, with fake time. */
#include <stdio.h>
#include "asteroid.h"
#include "power_policy.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static void nightstand(void)
{
	struct nightstand n = { 0 };
	CHECK(!nightstand_step(&n, false, 0, 5000));
	CHECK(nightstand_step(&n, true, 100, 5000));
	CHECK(nightstand_next_ms(&n, 100, 5000) == UINT32_MAX);
	/* Off the charger it counts for the delay. */
	CHECK(nightstand_step(&n, false, 1000, 5000));
	CHECK(nightstand_next_ms(&n, 2000, 5000) == 4000);
	CHECK(nightstand_step(&n, false, 5999, 5000));
	CHECK(!nightstand_step(&n, false, 6000, 5000));
	CHECK(nightstand_next_ms(&n, 6000, 5000) == UINT32_MAX);
	/* It stays expired when the clock wraps. */
	CHECK(!nightstand_step(&n, false, 1000 + 100, 5000));
	/* A zero delay ends it at once. */
	struct nightstand z = { 0 };
	nightstand_step(&z, true, 0, 0);
	CHECK(!nightstand_step(&z, false, 1, 0));
	/* Across the wrap of the clock. */
	struct nightstand w = { 0 };
	nightstand_step(&w, true, UINT32_MAX - 10, 100);
	CHECK(nightstand_step(&w, false, UINT32_MAX - 5, 100));
	CHECK(nightstand_step(&w, false, 50, 100));
	CHECK(!nightstand_step(&w, false, 95, 100));
}

static void timeout(void)
{
	struct power_policy p = { .awake = true };
	CHECK(power_policy_step(&p, true, SLEEP_AFTER_MS - 1) == POWER_STEP_NONE);
	CHECK(power_policy_step(&p, true, SLEEP_AFTER_MS) == POWER_STEP_SLEEP);
	CHECK(power_policy_step(&p, false, SLEEP_AFTER_MS) == POWER_STEP_NONE);
	/* Held awake it never times out, a request still works. */
	p.keep_awake = true;
	CHECK(power_policy_step(&p, true, 5 * SLEEP_AFTER_MS) == POWER_STEP_NONE);
	CHECK(power_policy_step(&p, true, 7 * SLEEP_AFTER_MS) == POWER_STEP_NONE);
	p.sleep_request = true;
	CHECK(power_policy_step(&p, true, 7 * SLEEP_AFTER_MS + 1) == POWER_STEP_SLEEP);
}

static void peek(void)
{
	/* Awake a peek is ignored; asleep it lasts PEEK_MS and another one
	 * starts it over. */
	struct power_policy p = { .awake = true };
	power_policy_peek(&p, 100);
	CHECK(!power_policy_peeking(&p, 100));
	CHECK(power_policy_peek_next_ms(&p, 100) == UINT32_MAX);
	p.awake = false;
	CHECK(!power_policy_peeking(&p, 100));
	power_policy_peek(&p, 1000);
	CHECK(power_policy_peeking(&p, 1000));
	CHECK(power_policy_peek_next_ms(&p, 1000) == PEEK_MS);
	CHECK(power_policy_peeking(&p, 1000 + PEEK_MS - 1));
	CHECK(power_policy_peek_next_ms(&p, 1000 + PEEK_MS - 1) == 1);
	power_policy_peek(&p, 1000 + PEEK_MS - 1);
	CHECK(power_policy_peeking(&p, 1000 + PEEK_MS));
	CHECK(!power_policy_peeking(&p, 1000 + 2 * PEEK_MS - 1));
	CHECK(power_policy_peek_next_ms(&p, 1000 + 2 * PEEK_MS) == UINT32_MAX);
	/* Waking ends it, and it does not come back when the watch sleeps. */
	power_policy_peek(&p, 20000);
	p.awake = true;
	power_policy_woke(&p, 20100);
	CHECK(!p.peeking);
	p.awake = false;
	CHECK(!power_policy_peeking(&p, 20200));
	/* Across the wrap of the clock. */
	power_policy_peek(&p, UINT32_MAX - 10);
	CHECK(power_policy_peeking(&p, PEEK_MS - 20));
	CHECK(power_policy_peek_next_ms(&p, PEEK_MS - 20) == 9);
	CHECK(!power_policy_peeking(&p, PEEK_MS - 11));
}

static void requests(void)
{
	/* Awake, both at once: sleep wins. Asleep, both at once: wake wins. */
	struct power_policy p = { .awake = true, .sleep_request = true, .wake_request = true };
	CHECK(power_policy_step(&p, true, 0) == POWER_STEP_SLEEP);
	p.awake = false;
	CHECK(power_policy_step(&p, true, 0) == POWER_STEP_WAKE);
	p.wake_request = false;
	CHECK(power_policy_step(&p, true, 0) == POWER_STEP_NONE);
}

static void charger(void)
{
	/* Asleep: a short wake. */
	struct power_policy p = { 0 };
	power_policy_charger(&p, 1000);
	CHECK(power_policy_step(&p, true, 1000) == POWER_STEP_WAKE);
	p.awake = true;
	power_policy_woke(&p, 1000);
	CHECK(!p.charger_wake);
	p.wake_request = false;
	CHECK(power_policy_step(&p, true, 1000 + CHARGER_WAKE_MS - 1) == POWER_STEP_NONE);
	CHECK(power_policy_step(&p, true, 1000 + CHARGER_WAKE_MS) == POWER_STEP_SLEEP);
	/* Asleep with another wake on its way: the full time. */
	struct power_policy q = { .wake_request = true };
	power_policy_charger(&q, 1000);
	CHECK(power_policy_step(&q, true, 1000) == POWER_STEP_WAKE);
	q.awake = true;
	power_policy_woke(&q, 1000);
	q.wake_request = false;
	CHECK(power_policy_step(&q, true, 1000 + SLEEP_AFTER_MS - 1) == POWER_STEP_NONE);
	/* Awake about to time out: at least the short wake. */
	struct power_policy r = { .awake = true };
	power_policy_charger(&r, SLEEP_AFTER_MS - 1);
	CHECK(power_policy_step(&r, true, SLEEP_AFTER_MS + CHARGER_WAKE_MS - 2) == POWER_STEP_NONE);
	CHECK(power_policy_step(&r, true, SLEEP_AFTER_MS + CHARGER_WAKE_MS - 1) == POWER_STEP_SLEEP);
}

static void aod(void)
{
	for (int m = 0; m < 8; m++) {
		bool ns = m & 1, ns_aod = m & 2, normal = m & 4;
		CHECK(power_policy_aod(ns, ns_aod, normal) == (ns ? ns_aod : normal));
	}
}

/* Asleep on the charger, then off it, for each pair of settings: what
 * the screen does on the way, with a 5 s nightstand delay. */
static void unplug(void)
{
	for (int m = 0; m < 4; m++) {
		bool ns_aod = m & 1, normal = m & 2, ambient = ns_aod;
		struct nightstand n = { 0 };
		nightstand_step(&n, true, 0, 5000);
		CHECK(power_policy_aod_change(ambient, power_policy_aod(true, ns_aod, normal)) == AOD_KEEP);
		bool attached = nightstand_step(&n, false, 1000, 5000);
		CHECK(power_policy_aod_change(ambient, power_policy_aod(attached, ns_aod, normal)) == AOD_KEEP);
		CHECK(nightstand_next_ms(&n, 1000, 5000) == 5000);
		attached = nightstand_step(&n, false, 6000, 5000);
		CHECK(!attached);
		enum aod_change c = power_policy_aod_change(ambient, power_policy_aod(attached, ns_aod, normal));
		CHECK(c == (ns_aod == normal ? AOD_KEEP : normal ? AOD_ENTER : AOD_LEAVE));
	}
}

static void touches(void)
{
	bool held = false;
	/* A tap on the sleeping watch wakes it, none of it reaches the UI. */
	CHECK(power_policy_touch(&held, false, true, true, false) == TOUCH_WAKE && held);
	CHECK(power_policy_touch(&held, true, true, false, false) == TOUCH_DROP && held);
	CHECK(power_policy_touch(&held, true, true, false, true) == TOUCH_DROP && !held);
	CHECK(power_policy_touch(&held, true, true, true, false) == TOUCH_USE);
	/* Lifted before the wake finished. */
	CHECK(power_policy_touch(&held, false, true, true, false) == TOUCH_WAKE);
	CHECK(power_policy_touch(&held, false, true, false, true) == TOUCH_DROP && !held);
	/* With tap to wake off nothing happens. */
	CHECK(power_policy_touch(&held, false, false, true, false) == TOUCH_DROP && !held);
}

int main(void)
{
	aod();
	unplug();
	touches();
	nightstand();
	peek();
	timeout();
	requests();
	charger();
	if (failures)
		return 1;
	puts("power_test: ok");
	return 0;
}
