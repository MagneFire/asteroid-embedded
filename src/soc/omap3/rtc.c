/* The wall clock. Neither the SoC nor the PMIC keeps one on this
 * board: the sensor hub does, so the clock here counts on from what
 * was last set, and the hub's driver sets it once the hub has started
 * and takes a new time along. It counts with the 32 kHz counter that
 * never stops, which goes round every day and a half: reading it more
 * often than that is what notices. */
#include "asteroid.h"

static uint32_t base, last, rounds;
static bool set;

bool rtc_valid(void) { return set; }

static uint32_t uptime_s(void)
{
	uint32_t flags = irq_save();
	uint32_t now = readl(SYNC32K_CR);
	if (now < last)
		rounds++;
	last = now;
	uint64_t count = (uint64_t)rounds << 32 | now;
	irq_restore(flags);
	return (uint32_t)(count / SLEEP_CLK_HZ);
}

uint32_t rtc_read(void)
{
	return base + uptime_s();
}

void rtc_write(uint32_t secs)
{
	base = secs - uptime_s();
	set = true;
}
