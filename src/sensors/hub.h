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
void hub_status(void (*say)(const char *fmt, ...));
/* A register access for the console: wn bytes written after the bank
 * and offset, then rn bytes read; -1 without such a hub. */
int hub_raw(uint8_t bank, uint8_t offset, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn);
#endif
