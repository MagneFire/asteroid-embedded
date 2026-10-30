/* Alarms: up to ALARMS of them in the key-value store, each a time, the
 * weekdays it repeats on (bit 0 Sunday, none for once) and whether it is
 * on. The RTC holds local time, so a day is rtc / 86400 and a weekday
 * follows from it. A once alarm turns itself off when it rings. Snoozing
 * rings again ten minutes later. */
#include <string.h>
#include "asteroid.h"
#include "alarm.h"
#include "storage/storage.h"

#define KV_ALARMS	"alarms"
#define SNOOZE_S	600

static struct alarm alarms[ALARMS];
static unsigned count;
static bool loaded;
static uint32_t last_minute, snooze_at;

static void load(void)
{
	if (loaded)
		return;
	uint8_t buf[1 + ALARMS * sizeof(struct alarm)];
	int n = kv_get(KV_ALARMS, buf, sizeof buf);
	if (n >= 1 && buf[0] <= ALARMS && n == (int)(1 + buf[0] * sizeof(struct alarm))) {
		count = buf[0];
		memcpy(alarms, buf + 1, count * sizeof(struct alarm));
	}
	loaded = true;
}

static void save(void)
{
	uint8_t buf[1 + ALARMS * sizeof(struct alarm)];
	buf[0] = (uint8_t)count;
	memcpy(buf + 1, alarms, count * sizeof(struct alarm));
	kv_set(KV_ALARMS, buf, 1 + count * sizeof(struct alarm));
}

unsigned alarm_count(void)
{
	load();
	return count;
}

const struct alarm *alarm_get(unsigned i)
{
	load();
	return i < count ? &alarms[i] : NULL;
}

/* index past the end adds one; returns false when there is no room. */
bool alarm_set(unsigned i, const struct alarm *a)
{
	load();
	if (i >= count) {
		if (count == ALARMS)
			return false;
		i = count++;
	}
	alarms[i] = *a;
	save();
	return true;
}

void alarm_delete(unsigned i)
{
	load();
	if (i >= count)
		return;
	memmove(&alarms[i], &alarms[i + 1], (count - i - 1) * sizeof(struct alarm));
	count--;
	save();
}

static bool due(const struct alarm *a, uint32_t minute)
{
	uint32_t day = minute / 1440, weekday = (day + 4) % 7, m = minute % 1440;
	return a->on && m == a->hour * 60u + a->minute && (!a->days || (a->days & (1u << weekday)));
}

/* Whether a snooze is over, or an alarm rings in the RTC minute rtc is
 * in, once per minute. */
bool alarm_check(uint32_t rtc)
{
	uint32_t minute = rtc / 60;
	if (snooze_at && rtc >= snooze_at) {
		snooze_at = 0;
		return true;
	}
	if (minute == last_minute)
		return false;
	last_minute = minute;
	bool ring = false;
	load();
	bool changed = false;
	for (unsigned i = 0; i < count; i++)
		if (due(&alarms[i], minute)) {
			ring = true;
			if (!alarms[i].days) {
				alarms[i].on = false;
				changed = true;
			}
		}
	if (changed)
		save();
	return ring;
}

void alarm_snooze(uint32_t rtc)
{
	snooze_at = rtc + SNOOZE_S;
}

/* Seconds until the next minute an alarm or a snooze could ring in. */
uint32_t alarm_next_s(uint32_t rtc)
{
	load();
	uint32_t best = UINT32_MAX, now = rtc / 60;
	if (snooze_at)
		best = snooze_at > rtc ? snooze_at - rtc : 0;
	for (unsigned i = 0; i < count; i++)
		for (uint32_t d = 0; d < 8 && alarms[i].on; d++) {
			uint32_t m = (now / 1440 + d) * 1440 + alarms[i].hour * 60u + alarms[i].minute;
			if (m > now && due(&alarms[i], m)) {
				if (m * 60 - rtc < best)
					best = m * 60 - rtc;
				break;
			}
		}
	return best;
}
