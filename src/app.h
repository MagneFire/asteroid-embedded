#ifndef APP_H
#define APP_H
#include <stdint.h>
#include <stdbool.h>
#include "power.h"

struct app_caps {
	bool display, ui, touch, haptics, hub, heart_rate;
};

void app_init(const struct app_caps *caps);
void app_open(int app);
void app_sensors(uint32_t now);
void app_flashlight(uint32_t now);
void app_touch(uint32_t now);
/* Each pass: its time and its slowest part, for the gesture line. */
void app_pass(uint32_t now, uint32_t us, const char *part, uint32_t woke_ms);
void app_cable(uint32_t now);
void app_key(uint32_t now, bool irq);
void app_tick(uint32_t now);
void app_ui_events(uint32_t now);
void app_after_events(uint32_t now);
void app_console_settings(void);
void app_power_changed(enum power_change change);
void app_hands(uint32_t now);
uint32_t app_next_ms(const char **why);
uint32_t app_awake_next_ms(void);
#endif
