/* APCS watchdog, counting the 32.768 kHz sleep clock. A bark raises an
 * interrupt (the main loop is stuck but interrupts still run: record where);
 * a bite resets the SoC through the secure world. */
#include "asteroid.h"

#define WDT_RST		(APCS_WDT_BASE + 0x04)
#define WDT_EN		(APCS_WDT_BASE + 0x08)
#define WDT_STS		(APCS_WDT_BASE + 0x0c)
#define WDT_BARK_TIME	(APCS_WDT_BASE + 0x10)
#define WDT_BITE_TIME	(APCS_WDT_BASE + 0x14)

/* The time registers hold 28 bits of sleep clock ticks. */
static uint32_t ticks(uint32_t ms)
{
	if (ms > WDT_MAX_MS)
		ms = WDT_MAX_MS;
	return (uint32_t)((uint64_t)ms * SLEEP_CLK_HZ / 1000);
}

void wdt_arm(uint32_t bark_ms, uint32_t bite_ms, void (*bark)(void))
{
	writel(0, WDT_EN);
	writel(1, WDT_RST);
	writel(ticks(bark_ms), WDT_BARK_TIME);
	writel(ticks(bite_ms), WDT_BITE_TIME);
	writel(1, WDT_EN);
	if (bark)
		gic_enable(IRQ_WDT_BARK, bark);
}

void wdt_feed(void)
{
	writel(1, WDT_RST);
}
