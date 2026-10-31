/* The clock apps: the timer, the stopwatch and the ringing of an alarm
 * or the timer, kept out of the main loop. clockapps_tick advances them
 * and says when the watch should wake for one; clockapps_event takes
 * the UI events they own; clockapps_refresh pushes their state to the
 * UI while the watch is awake; clockapps_sleep snoozes an alarm the
 * watch sleeps through; clockapps_next_ms says how long the watch may
 * sleep before one is due. */
#ifndef CLOCKAPPS_H
#define CLOCKAPPS_H
#include <stdint.h>
#include <stdbool.h>

struct ui_event;

void clockapps_init(bool have_ui, bool have_haptics);
bool clockapps_event(const struct ui_event *e, uint32_t now);
bool clockapps_tick(uint32_t now);
void clockapps_refresh(uint32_t now);
void clockapps_sleep(void);
uint32_t clockapps_next_ms(uint32_t now);
bool clockapps_ringing(void);
void clockapps_show_alarms(void);
#endif
