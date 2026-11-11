/* Time comes from GPTIMER2 counting the crystal, which stops with it in
 * a deep sleep: what the always-on 32 kHz counter saw pass meanwhile is
 * added on the way out (timer_slept). GPTIMER1 on the 32 kHz clock, in
 * the domain that stays up, gives the tick that wakes the main loop. */
#include "asteroid.h"
#include "prof.h"

#define TIOCP_CFG	0x10
#define TISTAT		0x14
#define TISR		0x18
#define TIER		0x1c
#define TWER		0x20
#define TCLR		0x24
#define TCRR		0x28
#define TLDR		0x2c
#define TWPS		0x34
#define TSICR		0x40
#define TCLR_ST		BIT(0)
#define TCLR_AR		BIT(1)
#define OVF		BIT(1)
#define TSICR_POSTED	BIT(2)
#define W_PEND_TCLR	BIT(0)
#define W_PEND_TCRR	BIT(1)
#define W_PEND_TLDR	BIT(2)

static uint32_t hz = 26000000;
static volatile uint32_t ticks;
/* The fast counter's overflows, and what deep sleeps add, in its ticks. */
static volatile uint32_t wraps;
static uint64_t slept;

static uint64_t cnt_now(void)
{
	uint32_t flags = irq_save();
	uint32_t lo = readl(GPT2_BASE + TCRR), hi = wraps;
	if (readl(GPT2_BASE + TISR) & OVF) {
		lo = readl(GPT2_BASE + TCRR);
		hi++;
	}
	uint64_t now = (((uint64_t)hi << 32) | lo) + slept;
	irq_restore(flags);
	return now;
}

/* An interrupt taken a second time for one overflow must not count. */
static void wrap_irq(void)
{
	if (!(readl(GPT2_BASE + TISR) & OVF))
		return;
	writel(OVF, GPT2_BASE + TISR);
	wraps++;
}

uint64_t timer_count(void) { return cnt_now(); }
uint32_t timer_hz(void) { return hz; }
uint32_t timer_ticks(void) { return ticks; }
uint32_t time_ms(void) { return (uint32_t)(cnt_now() / (hz / 1000)); }
uint32_t time_us(void) { return (uint32_t)(cnt_now() / (hz / 1000000)); }

void delay_us(uint32_t us)
{
	uint64_t end = cnt_now() + (uint64_t)us * (hz / 1000000) + 1;
	while (cnt_now() < end)
		;
}

void delay_ms(uint32_t ms) { delay_us(ms * 1000); }

/* The fast counter stood still for this many 32 kHz ticks. */
void timer_slept(uint32_t ticks32k)
{
	uint32_t flags = irq_save();
	slept += (uint64_t)ticks32k * hz / SLEEP_CLK_HZ;
	irq_restore(flags);
}

/* GPTIMER1's registers are written across to the 32 kHz side: a write
 * to one that has not taken the previous write yet is lost. */
static void tick_write(uint32_t reg, uint32_t pending, uint32_t v)
{
	while (readl(GPT1_BASE + TWPS) & pending)
		;
	writel(v, GPT1_BASE + reg);
}

static uint32_t tick_count(uint32_t ms)
{
	uint64_t n = (uint64_t)(ms ? ms : 1) * SLEEP_CLK_HZ / 1000;
	return n > 0xfffffffeu ? 0xfffffffeu : (uint32_t)n + 1;
}

void timer_set_period(uint32_t ms)
{
	tick_write(TLDR, W_PEND_TLDR, 0u - tick_count(ms));
}

void timer_next(uint32_t ms)
{
	tick_write(TCRR, W_PEND_TCRR, 0u - tick_count(ms));
}

static void tick_irq(void)
{
	if (!(readl(GPT1_BASE + TISR) & OVF))
		return;
	writel(OVF, GPT1_BASE + TISR);
	ticks++;
	prof_sample();
	wdt_tick();
}

void timer_resume(void) {}

void timer_init(void)
{
	hz = sys_clk_hz();
	/* GPTIMER2 from the crystal, GPTIMER1 from the 32 kHz clock. */
	writel(readl(CM_PER + CM_CLKSEL) | BIT(0), CM_PER + CM_CLKSEL);
	writel(readl(CM_WKUP + CM_CLKSEL) & ~BIT(0), CM_WKUP + CM_CLKSEL);
	clock_enable(CM_PER, PER_GPT2, true);
	clock_enable(CM_WKUP, WKUP_GPT1, true);

	writel(BIT(1), GPT2_BASE + TIOCP_CFG);
	while (!(readl(GPT2_BASE + TISTAT) & 1))
		;
	writel(0x3 << 8 | 0x2 << 3, GPT2_BASE + TIOCP_CFG);
	/* Its counter is read all the time, which posted mode does not
	 * allow. */
	writel(0, GPT2_BASE + TSICR);
	writel(0, GPT2_BASE + TLDR);
	writel(0, GPT2_BASE + TCRR);
	writel(OVF, GPT2_BASE + TIER);
	writel(TCLR_ST | TCLR_AR, GPT2_BASE + TCLR);
	irq_attach(IRQ_GPT2, wrap_irq);

	writel(BIT(1), GPT1_BASE + TIOCP_CFG);
	while (!(readl(GPT1_BASE + TISTAT) & 1))
		;
	/* Clocks kept in idle, wake-up on, no idle requests refused. */
	writel(0x3 << 8 | BIT(2) | 0x2 << 3, GPT1_BASE + TIOCP_CFG);
	writel(TSICR_POSTED, GPT1_BASE + TSICR);
	timer_set_period(1);
	timer_next(1);
	writel(OVF, GPT1_BASE + TIER);
	writel(OVF, GPT1_BASE + TWER);
	tick_write(TCLR, W_PEND_TCLR, TCLR_ST | TCLR_AR);
	irq_attach(IRQ_GPT1, tick_irq);
}
