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

#define SLEEP_AFTER_MS 10000
#define CHARGER_WAKE_MS 3000

enum power_change { POWER_NONE, POWER_SLEPT, POWER_WOKE };

void power_init(bool have_display, bool have_ui, bool have_hub);
void power_activity(uint32_t now);
void power_keep_awake(bool on);
void power_charger_plugged(uint32_t now);
enum power_change power_update(uint32_t *now);
bool power_awake(void);
bool power_nightstand(void);
void power_set_brightness(void);
void power_tick(void);
void power_idle(uint32_t app_next_ms);
uint32_t power_wakeups(void);
uint32_t power_slept_ms(uint32_t now);
#endif
