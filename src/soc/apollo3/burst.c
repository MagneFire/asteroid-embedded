/* TurboSPOT, the core at 96 MHz, around the UI's draws, and the flash
 * cache's counters around one draw. As Ambiq's HAL (am_hal_burst.c)
 * does it: the feature is asked for once, with the BLE buck trimmed for
 * it, and each burst is a request in FREQCTRL with the burst event on.
 * The clock changes from SRAM, where the HAL has the boot ROM write it:
 * not with the core fetching from flash as it changes. While the
 * debugger is about the core stays at 48 MHz, and its first clock edge
 * ends a burst (swdwake.c): SWD halts during a burst froze the core. */
#include <stdlib.h>
#include <string.h>
#include "asteroid.h"
#include "ui/ui.h"

#define MCUCTRL_SKU		(MCUCTRL_BASE + 0x14)
#define  SKU_ALLOWBURST		BIT(0)
#define  FEATURE_BURSTREQ	BIT(4)
#define  FEATURE_BURSTACK	BIT(5)
#define  FEATURE_BURSTAVAIL	BIT(6)
#define MCUCTRL_BLEBUCK2	(MCUCTRL_BASE + 0x368)
#define  BLEBUCK2_TON		0xfffu
#define  BLEBUCK2_TON_TRIM	(0x19u << 6 | 0x0cu)
#define CLKGEN_BLEBUCKTONADJ	(CLKGEN_BASE + 0x3c)
#define  TONADJUSTEN		BIT(22)
#define  FREQ_BURSTREQ		BIT(0)
#define  FREQ_BURSTACK		BIT(1)
#define  FREQ_BURSTSTATUS	BIT(2)
#define PWRCTRL_DEVPWREVENTEN	(PWRCTRL_BASE + 0x28)
#define  BURSTEVEN		BIT(31)
#define CACHECTRL_CACHECFG	(CACHECTRL_BASE + 0x00)
#define  CACHE_MONITOR		BIT(24)
#define CACHECTRL_CTRL		(CACHECTRL_BASE + 0x08)
#define  CTRL_RESET_STAT	BIT(1)
#define CACHECTRL_DMON(n)	(CACHECTRL_BASE + 0x40 + 4 * (n))
#define CACHECTRL_IMON(n)	(CACHECTRL_BASE + 0x50 + 4 * (n))
#define CHIPREV_A1		0x12u
#define FEATURE_WAIT_US		10000
/* Polls of FREQCTRL from SRAM, where time_us is out of reach: some
 * milliseconds, as the HAL's 10 ms wait. */
#define FREQ_POLLS		50000u
/* Ambiq's recommended wait states, and those out of reset. */
#define FLASH_WAIT_MIN		1
#define FLASH_WAIT_MAX		3

static bool avail, use = true, on, watch;
static uint32_t enters, enter_us, bursts, slow, for_swd, fails;

static __attribute__((section(".ramfunc"), noinline)) bool freq_set(uint32_t req)
{
	volatile uint32_t *freq = (volatile uint32_t *)CLKGEN_FREQCTRL;
	uint32_t want = req ? FREQ_BURSTSTATUS : 0;
	*freq = req;
	for (uint32_t i = 0; i < FREQ_POLLS; i++)
		if ((*freq & FREQ_BURSTSTATUS) == want)
			return true;
	return false;
}

void burst_init(void)
{
	if (!(readl(MCUCTRL_SKU) & SKU_ALLOWBURST))
		return;
	if ((readl(PWRCTRL_SUPPLYSRC) & SUPPLYSRC_BLEBUCKEN) && (readl(MCUCTRL_CHIPREV) & 0xff) >= CHIPREV_A1) {
		uint32_t flags = irq_save();
		writel((readl(MCUCTRL_BLEBUCK2) & ~BLEBUCK2_TON) | BLEBUCK2_TON_TRIM, MCUCTRL_BLEBUCK2);
		writel(readl(CLKGEN_BLEBUCKTONADJ) & ~TONADJUSTEN, CLKGEN_BLEBUCKTONADJ);
		irq_restore(flags);
	}
	writel(readl(MCUCTRL_FEATUREENABLE) | FEATURE_BURSTREQ, MCUCTRL_FEATUREENABLE);
	uint32_t t0 = time_us();
	while (!(readl(MCUCTRL_FEATUREENABLE) & FEATURE_BURSTACK))
		if (time_us() - t0 > FEATURE_WAIT_US)
			return;
	avail = (readl(MCUCTRL_FEATUREENABLE) & FEATURE_BURSTAVAIL) != 0;
}

void burst_log(void)
{
	log_printf("burst: 96 MHz %s\n", avail ? (use ? "around draws" : "off") : "not available");
}

/* Every pass enters, as whether it draws is only known after. */
static bool enter(void)
{
	uint32_t flags = irq_save();
	if (swd_wanted()) {
		irq_restore(flags);
		for_swd++;
		return false;
	}
	uint32_t t0 = time_us();
	writel(readl(PWRCTRL_DEVPWREVENTEN) | BURSTEVEN, PWRCTRL_DEVPWREVENTEN);
	on = freq_set(FREQ_BURSTREQ) && (readl(CLKGEN_FREQCTRL) & FREQ_BURSTACK);
	if (on) {
		cpu_hz = 2 * CORE_HZ;
	} else {
		freq_set(0);
		writel(readl(PWRCTRL_DEVPWREVENTEN) & ~BURSTEVEN, PWRCTRL_DEVPWREVENTEN);
		fails++;
	}
	enters++;
	enter_us += time_us() - t0;
	irq_restore(flags);
	return on;
}

void burst_leave(void)
{
	uint32_t flags = irq_save();
	if (on) {
		freq_set(0);
		writel(readl(PWRCTRL_DEVPWREVENTEN) & ~BURSTEVEN, PWRCTRL_DEVPWREVENTEN);
		cpu_hz = CORE_HZ;
		on = false;
	}
	irq_restore(flags);
}

bool burst_draw_begin(void)
{
	if (watch) {
		writel(CTRL_RESET_STAT, CACHECTRL_CTRL);
		writel(readl(CACHECTRL_CACHECFG) | CACHE_MONITOR, CACHECTRL_CACHECFG);
	}
	return avail && use && enter();
}

/* The counters: accesses, tag lookups, hits and line hits, for
 * instructions and for data. */
void burst_draw_end(bool drawn, bool fast, uint32_t us)
{
	burst_leave();
	if (drawn && fast)
		bursts++;
	else if (drawn)
		slow++;
	if (!watch || !drawn)
		return;
	watch = false;
	uint32_t i[4], d[4];
	for (unsigned k = 0; k < 4; k++) {
		i[k] = readl(CACHECTRL_IMON(k));
		d[k] = readl(CACHECTRL_DMON(k));
	}
	writel(readl(CACHECTRL_CACHECFG) & ~CACHE_MONITOR, CACHECTRL_CACHECFG);
	log_printf("cache: draw %lu us at %u MHz, flash wait %lu, cache %08lx: instructions %lu %lu %lu %lu, "
		   "data %lu %lu %lu %lu\n",
		   us, fast ? 96 : 48, flash_wait_now(), readl(CACHECTRL_CACHECFG), i[0], i[1], i[2],
		   i[3], d[0], d[1], d[2], d[3]);
}

/* burst [on|off]: 96 MHz around draws, and how many got it. */
void burst_command(const char *args, void (*say)(const char *fmt, ...))
{
	if (!strcmp(args, "on") || !strcmp(args, "off"))
		use = !strcmp(args, "on");
	else if (*args) {
		say("usage: burst [on|off]\n");
		return;
	}
	say("burst: %s, %lu draws at 96 MHz, %lu at 48; %lu passes kept at 48 for the debugger, %lu entered in %lu us, "
	    "%lu failed; freqctrl %08lx\n",
	    avail ? (use ? "on" : "off") : "not available", bursts, slow, for_swd, enters, enter_us, fails,
	    readl(CLKGEN_FREQCTRL));
}

/* cache [wait 1-3]: the cache's counters around the next draw, which is
 * of the whole screen, in the log; or the flash's read wait states. */
void cache_command(const char *args, void (*say)(const char *fmt, ...))
{
	if (!strncmp(args, "wait ", 5)) {
		uint32_t n = (uint32_t)strtoul(args + 5, NULL, 10);
		if (n < FLASH_WAIT_MIN || n > FLASH_WAIT_MAX) {
			say("usage: cache wait %u-%u\n", FLASH_WAIT_MIN, FLASH_WAIT_MAX);
			return;
		}
		flash_wait(n);
		say("cache: flash wait %lu\n", flash_wait_now());
		return;
	}
	if (*args) {
		say("usage: cache [wait %u-%u]\n", FLASH_WAIT_MIN, FLASH_WAIT_MAX);
		return;
	}
	watch = true;
	ui_mark_dirty(0, 0, board->panel.width, board->panel.height);
	say("cache: flash wait %lu, cache %08lx; counting the next draw, see the log\n", flash_wait_now(),
	    readl(CACHECTRL_CACHECFG));
}
