#ifndef NOTIFY_H
#define NOTIFY_H
#include <stdint.h>

#define NOTIFY_MAX	20

struct notification {
	int32_t id;
	uint32_t time, seq;
	char package[48], app[32], icon[32], vibrate[12];
	char summary[96], body[384];
};

void notify_feed(const uint8_t *p, unsigned n);
unsigned notify_count(void);
const struct notification *notify_get(unsigned i);
void notify_dismiss(int32_t id);
uint32_t notify_serial(void);
#endif
