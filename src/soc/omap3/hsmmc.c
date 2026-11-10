/* The eMMC's host on the OMAP3630, MMC2: eight data lines at 1.8 V,
 * the card's supplies always on, the pads muxed by the bootloader. The
 * registers from 0x100 are close to SDHCI's, as one 32-bit register
 * where SDHCI has two of 16, with the clock divider a plain 10 bits
 * from the 96 MHz clock and the eight-bit bus and open drain in a
 * register of their own. There is no DMA engine behind it, so data is
 * moved a block at a time through the data register. A transfer of
 * several blocks is ended with CMD12 from here. A status bit only
 * shows if its enable bit is set, though nothing is routed to the
 * interrupt line. */
#include <string.h>
#include "asteroid.h"
#include "storage/emmc_host.h"

#define MMC_SYSCONFIG	(MMC2_BASE + 0x010)
#define MMC_SYSSTATUS	(MMC2_BASE + 0x014)
#define MMC_CSRE	(MMC2_BASE + 0x024)
#define MMC_CON		(MMC2_BASE + 0x02c)
#define MMC_BLK		(MMC2_BASE + 0x104)
#define MMC_ARG		(MMC2_BASE + 0x108)
#define MMC_CMD		(MMC2_BASE + 0x10c)
#define MMC_RSP		(MMC2_BASE + 0x110)
#define MMC_DATA	(MMC2_BASE + 0x120)
#define MMC_PSTATE	(MMC2_BASE + 0x124)
#define MMC_HCTL	(MMC2_BASE + 0x128)
#define MMC_SYSCTL	(MMC2_BASE + 0x12c)
#define MMC_STAT	(MMC2_BASE + 0x130)
#define MMC_IE		(MMC2_BASE + 0x134)
#define MMC_ISE		(MMC2_BASE + 0x138)
#define MMC_CAPA	(MMC2_BASE + 0x140)

#define SYSCONFIG_RESET	BIT(1)
#define SYSCONFIG_RUN	(3u << 8 | 1u << 3)	/* both clocks kept, no idle */
#define CON_OD		BIT(0)
#define CON_INIT	BIT(1)
#define CON_DW8		BIT(5)
#define CMD_BCE		BIT(1)
#define CMD_DDIR	BIT(4)
#define CMD_MSBS	BIT(5)
#define CMD_ABORT	(3u << 22)
#define PSTATE_CMDI	BIT(0)
#define PSTATE_DATI	BIT(1)
#define HCTL_DTW	BIT(1)
#define HCTL_SDBP	BIT(8)
#define HCTL_1V8	(5u << 9)
#define SYSCTL_ICE	BIT(0)
#define SYSCTL_ICS	BIT(1)
#define SYSCTL_CEN	BIT(2)
#define SYSCTL_CLKD(n)	((n) << 6)
#define SYSCTL_DTO_MAX	(0xeu << 16)
#define SYSCTL_SRC	BIT(25)
#define SYSCTL_SRD	BIT(26)
#define STAT_CC		BIT(0)
#define STAT_TC		BIT(1)
#define STAT_BWR	BIT(4)
#define STAT_BRR	BIT(5)
#define STAT_ERRI	BIT(15)
#define STAT_CMD_ERRORS	(0xfu << 16)
#define STAT_SHOWN	0x317f0033u
#define CAPA_1V8	BIT(26)

#define FCLK_HZ		96000000u

static uint32_t clkd = 240;
static unsigned bus_width = 1;
static bool open_drain;

static bool wait_for(uint32_t addr, uint32_t mask, uint32_t want, uint32_t us)
{
	uint32_t t0 = time_us();
	while ((readl(addr) & mask) != want)
		if (time_us() - t0 > us)
			return false;
	return true;
}

/* The command or the data state machine back to idle after an error. */
static void lines_reset(uint32_t bits)
{
	writel(readl(MMC_SYSCTL) | bits, MMC_SYSCTL);
	wait_for(MMC_SYSCTL, bits, 0, 20000);
}

static int fail(const struct emmc_cmd *c, const char *what, uint32_t stat)
{
	log_printf("emmc: cmd%lu arg %08lx %s: stat %08lx pstate %08lx\n", c->index, c->arg, what, stat, readl(MMC_PSTATE));
	writel(~0u, MMC_STAT);
	lines_reset(SYSCTL_SRC | SYSCTL_SRD);
	return -(int)(stat >> 16 ? stat >> 16 : 0x10000);
}

static void power_and_clock(void)
{
	writel(SYSCONFIG_RUN, MMC_SYSCONFIG);
	writel(0, MMC_CSRE);
	writel(readl(MMC_CAPA) | CAPA_1V8, MMC_CAPA);
	writel(HCTL_1V8, MMC_HCTL);
	writel(HCTL_1V8 | HCTL_SDBP, MMC_HCTL);
	wait_for(MMC_HCTL, HCTL_SDBP, HCTL_SDBP, 20000);
	writel(STAT_SHOWN, MMC_IE);
	writel(0, MMC_ISE);
}

static int clock_set(void)
{
	writel(readl(MMC_SYSCTL) & ~SYSCTL_CEN, MMC_SYSCTL);
	writel(SYSCTL_CLKD(clkd) | SYSCTL_DTO_MAX | SYSCTL_ICE, MMC_SYSCTL);
	if (!wait_for(MMC_SYSCTL, SYSCTL_ICS, SYSCTL_ICS, 20000))
		return -2;
	writel(readl(MMC_SYSCTL) | SYSCTL_CEN, MMC_SYSCTL);
	return 0;
}

static void lines_set(void)
{
	writel((bus_width == 8 ? CON_DW8 : 0) | (open_drain ? CON_OD : 0), MMC_CON);
	uint32_t hctl = readl(MMC_HCTL) & ~HCTL_DTW;
	writel(bus_width == 4 ? hctl | HCTL_DTW : hctl, MMC_HCTL);
}

int emmc_host_clock(uint32_t hz, uint32_t *actual)
{
	clkd = (FCLK_HZ + hz - 1) / hz;
	if (clkd > 1023)
		clkd = 1023;
	if (clkd < 2)
		clkd = 2;
	*actual = FCLK_HZ / clkd;
	return clock_set();
}

int emmc_host_init(struct emmc_info *card)
{
	clock_enable(CM_CORE, CORE_MMC2, true);
	writel(SYSCONFIG_RESET, MMC_SYSCONFIG);
	if (!wait_for(MMC_SYSSTATUS, 1, 1, 100000)) {
		log_printf("emmc: host stays in reset\n");
		return -1;
	}
	bus_width = 1;
	open_drain = false;
	power_and_clock();
	lines_set();
	card->caps = readl(MMC_CAPA);
	log_printf("emmc: host caps %08lx, bus power %s\n", card->caps, readl(MMC_HCTL) & HCTL_SDBP ? "on" : "refused");
	if (!(readl(MMC_HCTL) & HCTL_SDBP))
		return -1;
	if (emmc_host_clock(400000, &card->clock_hz))
		return -2;
	/* The clocks a card wants before its first command. */
	writel(readl(MMC_CON) | CON_INIT, MMC_CON);
	writel(0, MMC_CMD);
	wait_for(MMC_STAT, STAT_CC, STAT_CC, 20000);
	writel(readl(MMC_CON) & ~CON_INIT, MMC_CON);
	writel(~0u, MMC_STAT);
	delay_ms(1);
	return 0;
}

static int issue(struct emmc_cmd *c, uint32_t extra)
{
	uint32_t busy = PSTATE_CMDI | (c->blocks || (c->flags & 3) == CMD_RESP_48_BUSY ? PSTATE_DATI : 0);
	if (!wait_for(MMC_PSTATE, busy, 0, 100000)) {
		log_printf("emmc: cmd%lu: lines busy %08lx\n", c->index, readl(MMC_PSTATE));
		lines_reset(SYSCTL_SRC | SYSCTL_SRD);
		return -1;
	}
	writel(~0u, MMC_STAT);
	uint32_t flags = c->flags, mode = 0;
	if (c->blocks) {
		writel(c->blocks << 16 | c->blksz, MMC_BLK);
		mode = (c->read ? CMD_DDIR : 0) | (c->blocks > 1 ? CMD_MSBS | CMD_BCE : 0);
		flags |= CMD_DATA;
	}
	writel(c->arg, MMC_ARG);
	writel(c->index << 24 | flags << 16 | mode | extra, MMC_CMD);
	uint32_t stat, t0 = time_us();
	while (!((stat = readl(MMC_STAT)) & (STAT_CC | STAT_ERRI)))
		if (time_us() - t0 > 100000)
			return fail(c, "no response", stat);
	if (stat & STAT_CMD_ERRORS)
		return fail(c, "error", stat);
	writel(STAT_CC, MMC_STAT);
	for (int i = 0; i < ((flags & 3) == CMD_RESP_136 ? 4 : 1); i++)
		c->resp[i] = readl(MMC_RSP + 4 * (uint32_t)i);
	if (!c->blocks && (flags & 3) != CMD_RESP_48_BUSY)
		return 0;
	uint32_t limit = c->timeout_us ? c->timeout_us : 1000000, block = 0, words = c->blksz / 4;
	uint32_t *p = (uint32_t *)(void *)emmc_bounce;
	t0 = time_us();
	for (;;) {
		stat = readl(MMC_STAT);
		if (stat & STAT_ERRI)
			return fail(c, "transfer error", stat);
		if (block < c->blocks && (stat & (c->read ? STAT_BRR : STAT_BWR))) {
			writel(c->read ? STAT_BRR : STAT_BWR, MMC_STAT);
			for (uint32_t i = 0; i < words; i++) {
				if (c->read)
					*p++ = readl(MMC_DATA);
				else
					writel(*p++, MMC_DATA);
			}
			block++;
			continue;
		}
		if (block == c->blocks && (stat & STAT_TC))
			break;
		if (time_us() - t0 > limit)
			return fail(c, "transfer timeout", stat);
		wdt_feed();
	}
	writel(STAT_TC, MMC_STAT);
	return 0;
}

int emmc_host_send(struct emmc_cmd *c)
{
	if (c->blocks && (c->blksz & 3))
		return -1;
	int r = issue(c, 0);
	if (c->blocks > 1) {
		struct emmc_cmd stop = { .index = 12, .flags = c->read ? R1 : R1B };
		int s = issue(&stop, CMD_ABORT);
		if (!r)
			r = s;
	}
	return r;
}

void emmc_host_open_drain(bool on)
{
	open_drain = on;
	lines_set();
}

/* The card is clocked at 48 MHz with the timing it starts with. */
void emmc_host_high_speed(void) {}

unsigned emmc_host_max_width(void) { return 8; }

void emmc_host_set_width(unsigned bits)
{
	bus_width = bits;
	lines_set();
}

/* A deep sleep of the chip may take the controller's settings: bus
 * power off says so. */
int emmc_host_rest(bool on)
{
	if (on) {
		writel(readl(MMC_SYSCTL) & ~SYSCTL_CEN, MMC_SYSCTL);
		clock_enable(CM_CORE, CORE_MMC2, false);
		return 0;
	}
	clock_enable(CM_CORE, CORE_MMC2, true);
	if (readl(MMC_HCTL) & HCTL_SDBP) {
		writel(readl(MMC_SYSCTL) | SYSCTL_CEN, MMC_SYSCTL);
		return 0;
	}
	power_and_clock();
	lines_set();
	return clock_set() ? -10 : 0;
}
