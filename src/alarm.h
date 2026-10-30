#ifndef ALARM_H
#define ALARM_H
#include <stdint.h>
#include <stdbool.h>

#define ALARMS 8

struct alarm {
	uint8_t hour, minute, days;
	bool on;
};

unsigned alarm_count(void);
const struct alarm *alarm_get(unsigned i);
bool alarm_set(unsigned i, const struct alarm *a);
void alarm_delete(unsigned i);
bool alarm_check(uint32_t rtc);
void alarm_snooze(uint32_t rtc);
uint32_t alarm_next_s(uint32_t rtc);
#endif
