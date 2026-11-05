/* What the log says about the running system: each second awake, each
 * minute asleep. A build-time TEST hangs or faults here on purpose to
 * exercise the crash path. */
#include "asteroid.h"
#include "display/display.h"
#include "input/touch.h"
#include "frame.h"
#include "power.h"
#include "stats.h"
#include "config.h"

#define ASLEEP_STATS_MS 60000

static uint32_t start_ms, last, frames0;

void stats_init(uint32_t start)
{
	start_ms = start;
	frames0 = display_frames();
}

void stats_poll(uint32_t now, bool awake)
{
	if (!awake && now - last >= ASLEEP_STATS_MS) {
		last = now;
		uint32_t pc, pcc, pcb;
		spm_stats(&pc, &pcc, &pcb);
		log_printf("power: asleep, %lu wakeups, %lu/%lu/%lu collapses, %ld mA average\n", power_wakeups(), pc, pcc,
			   pcb, pmic_battery_avg_ua() / 1000);
	}
	if (awake && now - last >= 1000) {
		last = now;
		uint32_t rx, tx, att, terr;
		usb_stats(&rx, &tx);
		touch_stats(&att, &terr);
		log_printf("tick %lu, %lu fps, rtc %lu, battery %lu mV, %s, usb %lu/%lu, touch %lu/%lu\n", (now - start_ms) / 1000,
			   display_frames() - frames0, pmic_rtc_read(), pmic_battery_mv(), pmic_charger_status(), rx, tx, att, terr);
		frames0 = display_frames();
		uint32_t slept = power_slept_ms(now);
		uint32_t pc, pcc, pcb;
		spm_stats(&pc, &pcc, &pcb);
		log_printf("power: %s, asleep %lu%% of the last second, %lu wakeups, %lu/%lu/%lu collapses, battery %ld mA, %ld mA average\n",
			   awake ? "awake" : "asleep", slept > 1000 ? 100 : slept / 10, power_wakeups(), pc, pcc, pcb,
			   pmic_battery_ua() / 1000, pmic_battery_avg_ua() / 1000);
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
