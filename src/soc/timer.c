/* ARM generic timer. CNTFRQ is set by the secure world (19.2 MHz) and is
 * read-only here. A 1 ms tick on the non-secure physical timer wakes the
 * main loop out of WFI; time stamps read the counter directly. The
 * timer's PPIs are wired as 18 (secure) and 19 (non-secure) on this SoC,
 * not the architectural 29 and 30. */
#include "asteroid.h"

static uint32_t hz = 19200000;
static volatile uint32_t ticks;
static uint32_t period_ms = 1;

static inline uint64_t cnt_now(void)
{
	uint32_t lo, hi;
	__asm__ volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
	return ((uint64_t)hi << 32) | lo;
}

uint64_t timer_count(void) { return cnt_now(); }
uint32_t timer_hz(void) { return hz; }
uint32_t timer_ticks(void) { return ticks; }
uint32_t time_ms(void) { return (uint32_t)(cnt_now() / (hz / 1000)); }
uint32_t time_us(void) { return (uint32_t)(cnt_now() / (hz / 1000000)); }

void delay_us(uint32_t us)
{
	uint64_t end = cnt_now() + (uint64_t)us * (hz / 1000000);
	while (cnt_now() < end)
		;
}

void delay_ms(uint32_t ms) { delay_us(ms * 1000); }

void timer_set_period(uint32_t ms)
{
	period_ms = ms ? ms : 1;
}

static void tick_irq(void)
{
	uint32_t tval = hz / 1000 * period_ms;
	__asm__ volatile("mcr p15, 0, %0, c14, c2, 0" :: "r"(tval));	/* CNTP_TVAL */
	ticks++;
}

/* After a power collapse: the core's timer starts out disabled. */
void timer_resume(void)
{
	uint32_t tval = 1, ctl = 1;
	__asm__ volatile("mcr p15, 0, %0, c14, c2, 0" :: "r"(tval));
	__asm__ volatile("mcr p15, 0, %0, c14, c2, 1" :: "r"(ctl));
}

void timer_init(void)
{
	uint32_t frq;
	__asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(frq));	/* CNTFRQ */
	if (frq)
		hz = frq;
	uint32_t tval = hz / 1000, ctl = 1;
	__asm__ volatile("mcr p15, 0, %0, c14, c2, 0" :: "r"(tval));
	__asm__ volatile("mcr p15, 0, %0, c14, c2, 1" :: "r"(ctl));	/* CNTP_CTL: enable */
	gic_enable(IRQ_PPI_TIMER, tick_irq);
	gic_enable(IRQ_PPI_TIMER_S, tick_irq);
}
