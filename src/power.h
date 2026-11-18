/* The sleep and wake policy: the watch sleeps SLEEP_AFTER_MS after the
 * last activity or on request, into the always-on face (ambient) or
 * with the panel off, and wakes on request. power_update makes the
 * transition and says which; power_idle waits for the next thing to
 * do, parking the cluster when it can; power_activity and
 * power_request are the inputs; power_keep_awake holds off the timeout
 * (not a request) for testing over the console. */
#ifndef POWER_H
#define POWER_H
#include <stdint.h>
#include <stdbool.h>
#include "power_policy.h"

enum power_change { POWER_NONE, POWER_SLEPT, POWER_WOKE };

void power_init(bool have_display, bool have_ui, bool have_hub);
void power_activity(uint32_t now);
void power_keep_awake(bool on);
void power_charger_plugged(uint32_t now);
enum power_change power_update(uint32_t *now);
bool power_awake(void);
bool power_nightstand(void);
uint32_t power_next_ms(void);
void power_set_brightness(void);
/* Each pass: awake, the brightness follows the light sensor, unless
 * something else is holding the brightness. */
void power_light(uint32_t now, bool hold);
void power_tick(void);
void power_idle(uint32_t app_next_ms, const char *why);
void power_timepiece(void) __attribute__((noreturn));
uint32_t power_wakeups(void);
/* What ended the sleeps since the last call, most first. */
const char *power_woke_by(void);
uint32_t power_slept_ms(uint32_t now);
/* For tuning over the console: the last peek's lux and light, and a
 * light that every peek uses in place of the one for the room. */
int32_t power_backlight_seen(struct backlight *b);
void power_backlight_fix(const struct backlight *b);
/* Awake, the time the core waited with nothing to do, since the last
 * call. */
void power_count_idle(uint32_t us);
uint32_t power_idle_us(void);
/* While the application processor has the panel the watch stays
 * asleep with the screen off. */
void power_lend_display(bool lent);
/* The screen stays off until the next wake. */
void power_dark_until_wake(void);
#endif
