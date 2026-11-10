/* Watchdog timer 2 on the 32 kHz clock resets the chip when it runs
 * out. It has no interrupt ahead of that, so the bark is the tick
 * interrupt noticing that the main loop has not fed it for the bark
 * time: with interrupts off there is the reset alone. */
#include "asteroid.h"

#define WD_SYSCONFIG	(WDT2_BASE + 0x10)
#define WD_SYSSTATUS	(WDT2_BASE + 0x14)
#define WCLR		(WDT2_BASE + 0x24)
#define WCRR		(WDT2_BASE + 0x28)
#define WLDR		(WDT2_BASE + 0x2c)
#define WTGR		(WDT2_BASE + 0x30)
#define WWPS		(WDT2_BASE + 0x34)
#define WSPR		(WDT2_BASE + 0x48)

static void (*bark_fn)(void);
static uint32_t bark_ms, fed_at, trigger;
static bool armed;

static void settle(void)
{
	while (readl(WWPS) & 0x3f)
		;
}

static void enable(bool on)
{
	writel(on ? 0xbbbb : 0xaaaa, WSPR);
	settle();
	writel(on ? 0x4444 : 0x5555, WSPR);
	settle();
}

void wdt_feed(void)
{
	fed_at = time_ms();
	/* A feed that is still on its way to the 32 kHz side is enough. */
	if (!(readl(WWPS) & BIT(3)))
		writel(++trigger, WTGR);
}

/* From the tick interrupt. */
void wdt_tick(void)
{
	if (armed && bark_fn && time_ms() - fed_at >= bark_ms) {
		void (*fn)(void) = bark_fn;
		bark_fn = NULL;
		fn();
	}
}

void wdt_arm(uint32_t bark, uint32_t bite_ms, void (*fn)(void))
{
	clock_enable(CM_WKUP, WKUP_WDT2, true);
	armed = false;
	enable(false);
	if (bite_ms > WDT_MAX_MS)
		bite_ms = WDT_MAX_MS;
	uint32_t count = (uint32_t)((uint64_t)bite_ms * SLEEP_CLK_HZ / 1000);
	/* No prescaler: one count per 32 kHz tick. */
	writel(0, WCLR);
	settle();
	writel(0u - count, WLDR);
	settle();
	writel(0u - count, WCRR);
	settle();
	if (fn)
		bark_fn = fn;
	bark_ms = bark;
	wdt_feed();
	settle();
	enable(true);
	armed = true;
}

void wdt_stop(void)
{
	armed = false;
	enable(false);
}
