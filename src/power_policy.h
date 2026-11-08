/* The decisions behind sleep and wake, without any hardware, so they can
 * be tested on the host. power.c carries them out. */
#ifndef POWER_POLICY_H
#define POWER_POLICY_H
#include <stdint.h>
#include <stdbool.h>

#define SLEEP_AFTER_MS 10000
#define CHARGER_WAKE_MS 3000
#define PEEK_MS 5000

struct nightstand {
	bool was_usb, lost;
	uint32_t lost_t;
};

/* Whether the cable counts as attached, feeding in its state now. */
bool nightstand_step(struct nightstand *n, bool usb, uint32_t now, uint32_t delay_ms);
/* The time until a removed cable stops counting. */
uint32_t nightstand_next_ms(const struct nightstand *n, uint32_t now, uint32_t delay_ms);

struct power_policy {
	bool awake, sleep_request, wake_request, charger_wake, keep_awake, peeking;
	uint32_t last_activity, peek_t;
};

enum power_step { POWER_STEP_NONE, POWER_STEP_SLEEP, POWER_STEP_WAKE };

/* A cable plugged in shows the screen for CHARGER_WAKE_MS. */
void power_policy_charger(struct power_policy *p, uint32_t now);
/* What to do this pass. The caller clears the requests afterwards. */
enum power_step power_policy_step(struct power_policy *p, bool have_display, uint32_t now);
/* Which setting picks the always-on face. */
bool power_policy_aod(bool nightstand, bool nightstand_aod, bool aod);
/* A peek: asleep, the always-on face for PEEK_MS from `now`, whatever
 * the setting; another peek starts it over. Awake it does nothing. */
void power_policy_peek(struct power_policy *p, uint32_t now);
/* Whether a peek is still on at `now`. */
bool power_policy_peeking(struct power_policy *p, uint32_t now);
/* The time until the peek ends, UINT32_MAX without one. */
uint32_t power_policy_peek_next_ms(const struct power_policy *p, uint32_t now);

enum aod_change { AOD_KEEP, AOD_ENTER, AOD_LEAVE };

/* Asleep, what the always-on face has to do once the wanted state is
 * known. */
enum aod_change power_policy_aod_change(bool ambient, bool wanted);

enum touch_use { TOUCH_USE, TOUCH_DROP, TOUCH_WAKE };

/* What a touch does: asleep it may wake the watch, and that finger is
 * then kept from the UI until it lifts. */
enum touch_use power_policy_touch(bool *wake_touch, bool awake, bool tap_wakes, bool down, bool up);
/* After waking: when the screen goes off again counts from `now`. */
void power_policy_woke(struct power_policy *p, uint32_t now);
#endif
