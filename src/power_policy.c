#include "asteroid.h"
#include "power_policy.h"

bool nightstand_step(struct nightstand *n, bool usb, uint32_t now, uint32_t delay_ms)
{
	if (n->was_usb && !usb) {
		n->lost_t = now;
		n->lost = true;
	}
	n->was_usb = usb;
	if (n->lost && now - n->lost_t >= delay_ms)
		n->lost = false;
	return usb || n->lost;
}

uint32_t nightstand_next_ms(const struct nightstand *n, uint32_t now, uint32_t delay_ms)
{
	return n->lost ? ms_left(n->lost_t, delay_ms, now) : UINT32_MAX;
}

void power_policy_charger(struct power_policy *p, uint32_t now)
{
	if (!p->awake)
		p->charger_wake = true;
	else if (now - p->last_activity > SLEEP_AFTER_MS - CHARGER_WAKE_MS)
		p->last_activity = now - (SLEEP_AFTER_MS - CHARGER_WAKE_MS);
}

enum power_step power_policy_step(struct power_policy *p, bool have_display, uint32_t now)
{
	/* The short charger wake only when nothing else wakes the watch. */
	if (p->charger_wake && !p->awake && !p->wake_request)
		p->wake_request = true;
	else
		p->charger_wake = false;
	if (p->keep_awake && p->awake && !p->sleep_request)
		p->last_activity = now;
	if (have_display && p->awake && (p->sleep_request || now - p->last_activity >= SLEEP_AFTER_MS))
		return POWER_STEP_SLEEP;
	if (have_display && !p->awake && p->wake_request)
		return POWER_STEP_WAKE;
	return POWER_STEP_NONE;
}

bool power_policy_aod(bool nightstand, bool nightstand_aod, bool aod)
{
	return nightstand ? nightstand_aod : aod;
}

void power_policy_peek(struct power_policy *p, uint32_t now)
{
	if (p->awake)
		return;
	p->peeking = true;
	p->peek_t = now;
}

/* Signed: the peek may be stamped after the pass's own time. */
static int32_t peek_gone(const struct power_policy *p, uint32_t now)
{
	return (int32_t)(now - p->peek_t);
}

bool power_policy_peeking(struct power_policy *p, uint32_t now)
{
	if (p->peeking && (p->awake || peek_gone(p, now) >= (int32_t)PEEK_MS))
		p->peeking = false;
	return p->peeking;
}

uint32_t power_policy_peek_next_ms(const struct power_policy *p, uint32_t now)
{
	if (!p->peeking || p->awake)
		return UINT32_MAX;
	int32_t gone = peek_gone(p, now);
	return gone >= (int32_t)PEEK_MS ? 1 : (uint32_t)((int32_t)PEEK_MS - gone);
}

enum aod_change power_policy_aod_change(bool ambient, bool wanted)
{
	if (ambient == wanted)
		return AOD_KEEP;
	return wanted ? AOD_ENTER : AOD_LEAVE;
}

enum touch_use power_policy_touch(bool *wake_touch, bool awake, bool tap_wakes, bool down, bool up)
{
	if (!awake) {
		if (down && tap_wakes) {
			*wake_touch = true;
			return TOUCH_WAKE;
		}
		if (up)
			*wake_touch = false;
		return TOUCH_DROP;
	}
	if (*wake_touch) {
		*wake_touch = !up;
		return TOUCH_DROP;
	}
	return TOUCH_USE;
}

void power_policy_woke(struct power_policy *p, uint32_t now)
{
	p->last_activity = now;
	p->peeking = false;
	if (p->charger_wake)
		p->last_activity -= SLEEP_AFTER_MS - CHARGER_WAKE_MS;
	p->charger_wake = false;
}
