/* What the log says about the running system: each second awake, and
 * asleep at most each minute, on a wake for something else unless the
 * build has ASLEEP_STATS. A build-time TEST hangs or faults here on
 * purpose to exercise the crash path. */
#include "asteroid.h"
#include "display/display.h"
#include "input/touch.h"
#include "frame.h"
#include "power.h"
#include "stats.h"
#include "config.h"
#include "bt/bt.h"

#define ASLEEP_STATS_MS 60000

static uint32_t start_ms, last, frames0;

void stats_init(uint32_t start)
{
	start_ms = start;
	frames0 = display_frames();
}

/* A gauge without an average of its own gives a reading that can be
 * minutes old while asleep, so the line says how old it is. */
static const char *current(bool now_too)
{
	static char s[64];
	if (!batt_ua_aged()) {
		if (now_too)
			snprintf(s, sizeof s, "battery %ld mA, %ld mA average", batt_ua() / 1000, batt_avg_ua() / 1000);
		else
			snprintf(s, sizeof s, "%ld mA average", batt_avg_ua() / 1000);
	} else if (batt_ua_age_ms() == UINT32_MAX) {
		snprintf(s, sizeof s, "no battery current yet");
	} else {
		snprintf(s, sizeof s, "battery %ld mA read %lu s ago", batt_ua() / 1000, batt_ua_age_ms() / 1000);
	}
	return s;
}

static void asleep_line(uint32_t now)
{
	uint32_t pc, pcc, pcb;
	cpu_idle_stats(&pc, &pcc, &pcb);
	log_printf("power: asleep %lu s, %lu wakeups, %lu/%lu/%lu collapses, %s\n", (now - last) / 1000, power_wakeups(), pc,
		   pcc, pcb, current(false));
	log_printf("power: woke for %s\n", power_woke_by());
	bt_transport_log();
	last = now;
}

/* The pass that wakes the watch is already awake for stats_poll. */
void stats_woke(uint32_t now)
{
	if (now - last >= 1000)
		asleep_line(now);
}

void stats_poll(uint32_t now, bool awake)
{
	if (!awake && now - last >= ASLEEP_STATS_MS)
		asleep_line(now);
	if (awake && now - last >= 1000) {
		last = now;
		uint32_t rx, tx, att, terr;
		usb_stats(&rx, &tx);
		touch_stats(&att, &terr);
		log_printf("tick %lu, %lu fps, rtc %lu, battery %lu mV, %s, usb %lu/%lu, touch %lu/%lu\n", (now - start_ms) / 1000,
			   display_frames() - frames0, rtc_read(), batt_mv(), batt_status(), rx, tx, att, terr);
		frames0 = display_frames();
		uint32_t slept = power_slept_ms(now);
		uint32_t pc, pcc, pcb;
		cpu_idle_stats(&pc, &pcc, &pcb);
		log_printf("power: %s, asleep %lu%% of the last second, %lu wakeups, %lu/%lu/%lu collapses, %s\n",
			   awake ? "awake" : "asleep", slept > 1000 ? 100 : slept / 10, power_wakeups(), pc, pcc, pcb,
			   current(true));
		frame_log();
#if defined(TEST_HANG)
		if (now - start_ms >= 2000) {
			log_printf("test: hanging the main loop\n");
			for (;;)
				;
		}
#elif defined(TEST_ABORT)
		if (now - start_ms >= 2000) {
			log_printf("test: data abort\n");
			*(volatile uint32_t *)0xe0000000u = 1;
		}
#endif
	}
}

/* The time to the next line asleep. */
uint32_t stats_next_ms(void)
{
	uint32_t stats = ASLEEP_STATS_MS - (time_ms() - last);
	return stats > ASLEEP_STATS_MS ? 1 : stats;
}
