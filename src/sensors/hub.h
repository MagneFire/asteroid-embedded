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
uint32_t hub_heart_rate(uint8_t *status);
bool hub_tilted(void);
bool hub_can_sleep(void);
void hub_sleep(bool asleep);
void hub_status(void (*say)(const char *fmt, ...));
#endif
