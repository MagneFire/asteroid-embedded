#ifndef HUB_H
#define HUB_H
#include <stdbool.h>
#include <stdint.h>

bool hub_init(void);
void hub_poll(void);
int hub_send(const uint8_t *p, unsigned n);
#define HUB_NONE	0xffffffffu
void hub_set_time(void);
uint32_t hub_steps(void);
/* What the hub makes of the steps, where it measures that itself from
 * the wearer's profile: distance in m and kcal since boot. */
struct hub_profile {
	uint8_t age, height_cm, weight_kg;
	bool male;
};
bool hub_measures_activity(void);
bool hub_activity(uint32_t v[2]);
/* The heart rate the hub logs by itself, where it does: on or off, and
 * the readings it took since the last call, oldest first. */
bool hub_has_passive(void);
void hub_passive_on(bool on);
unsigned hub_passive(uint32_t *t, uint8_t *bpm, unsigned max);
void hub_profile(const struct hub_profile *p);
void hub_heart_rate_on(bool on);
uint32_t hub_heart_rate(uint8_t *status, unsigned *readings);
bool hub_tilted(void);
/* Tilt detection, off with the screen on as the vendor has it; false
 * if the hub did not take it, to be tried again. */
bool hub_tilt_on(bool on);
void hub_accel_on(bool on);
bool hub_accel(int16_t v[3]);
bool hub_can_sleep(void);
void hub_sleep(bool asleep);
void hub_screen(bool on);
void hub_status(void (*say)(const char *fmt, ...));
/* A register access for the console: wn bytes written after the bank
 * and offset, then rn bytes read; -1 without such a hub. */
int hub_raw(uint8_t bank, uint8_t offset, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn);
#endif
