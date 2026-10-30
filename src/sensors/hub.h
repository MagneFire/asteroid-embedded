#ifndef HUB_H
#define HUB_H
#include <stdbool.h>
#include <stdint.h>

bool hub_init(void);
void hub_poll(void);
int hub_send(const uint8_t *p, unsigned n);
bool hub_tilted(void);
bool hub_can_sleep(void);
void hub_sleep(bool asleep);
void hub_status(void (*say)(const char *fmt, ...));
#endif
