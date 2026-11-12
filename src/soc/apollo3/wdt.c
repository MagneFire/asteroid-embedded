/* The watchdog on its 16 Hz clock: an interrupt at the bark count, a
 * reset at the bite count, both in 8 bits. */
#include "asteroid.h"

#define WDT_CFG		(WDT_BASE + 0x00)
#define WDT_RSTRT	(WDT_BASE + 0x04)
#define WDT_INTEN	(WDT_BASE + 0x200)
#define WDT_INTCLR	(WDT_BASE + 0x208)
#define RSTGEN_CFG	(RSTGEN_BASE + 0x00)
#define CLK_16HZ	2u
#define CFG_WDTEN	BIT(0)
#define CFG_INTEN	BIT(1)
#define CFG_RESEN	BIT(2)
#define RESTART_KEY	0xb2u
#define RSTGEN_WDREN	BIT(1)

static void (*bark_fn)(void);

static uint32_t counts(uint32_t ms)
{
	uint32_t n = ms * 16 / 1000;
	return n < 1 ? 1 : n > 255 ? 255 : n;
}

static void bark_irq(void)
{
	writel(1, WDT_INTCLR);
	if (bark_fn) {
		void (*fn)(void) = bark_fn;
		bark_fn = NULL;
		fn();
	}
}

void wdt_feed(void) { writel(RESTART_KEY, WDT_RSTRT); }

void wdt_arm(uint32_t bark_ms, uint32_t bite_ms, void (*fn)(void))
{
	writel(0, WDT_CFG);
	uint32_t bite = counts(bite_ms), bark = fn ? counts(bark_ms) : 0;
	if (bark >= bite)
		bark = bite > 1 ? bite - 1 : 0;
	if (fn)
		bark_fn = fn;
	writel(readl(RSTGEN_CFG) | RSTGEN_WDREN, RSTGEN_CFG);
	writel(1, WDT_INTCLR);
	writel(bark ? 1 : 0, WDT_INTEN);
	writel(CLK_16HZ << 24 | bark << 16 | bite << 8 | CFG_RESEN | (bark ? CFG_INTEN : 0) | CFG_WDTEN, WDT_CFG);
	irq_attach(IRQ_WDT, bark_irq);
	wdt_feed();
}

void wdt_stop(void)
{
	writel(0, WDT_CFG);
	irq_detach(IRQ_WDT);
}
