/* APCS watchdog, counting the 32.768 kHz sleep clock. A bark raises an
 * interrupt (the main loop is stuck but interrupts still run: record where);
 * a bite resets the SoC through the secure world. */
#include "asteroid.h"

#define WDT_RST		(APCS_WDT_BASE + 0x04)
#define WDT_EN		(APCS_WDT_BASE + 0x08)
#define WDT_STS		(APCS_WDT_BASE + 0x0c)
#define WDT_BARK_TIME	(APCS_WDT_BASE + 0x10)
#define WDT_BITE_TIME	(APCS_WDT_BASE + 0x14)

void wdt_arm(uint32_t bark_ms, uint32_t bite_ms, void (*bark)(void))
{
	writel(0, WDT_EN);
	writel(1, WDT_RST);
	writel(bark_ms * SLEEP_CLK_HZ / 1000, WDT_BARK_TIME);
	writel(bite_ms * SLEEP_CLK_HZ / 1000, WDT_BITE_TIME);
	writel(1, WDT_EN);
	if (bark)
		gic_enable(IRQ_WDT_BARK, bark);
}

void wdt_feed(void)
{
	writel(1, WDT_RST);
}

void wdt_stop(void)
{
	writel(0, WDT_EN);
}
