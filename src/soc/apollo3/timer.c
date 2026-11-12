/* Time is the system timer counting the 32 kHz crystal, which runs on in
 * deep sleep; its overflows, every 36 hours, are counted in its
 * interrupt. Its first compare gives the tick that wakes the main loop.
 * Short delays count core cycles instead, for their resolution. The wall
 * clock counts on from what was last set, like a clock that keeps no
 * time of its own: the phone sets it on connecting. */
#include "asteroid.h"
#include "prof.h"

#define STCFG		(CTIMER_BASE + 0x140)
#define STTMR		(CTIMER_BASE + 0x144)
#define SCMPR(n)	(CTIMER_BASE + 0x150 + 4 * (n))
#define STMINTEN	(CTIMER_BASE + 0x300)
#define STMINTSTAT	(CTIMER_BASE + 0x304)
#define STMINTCLR	(CTIMER_BASE + 0x308)
#define STCFG_XTAL	3u
#define STCFG_COMPARE_A	BIT(8)
#define STCFG_CLEAR	BIT(30)
#define STCFG_FREEZE	BIT(31)
#define INT_COMPAREA	BIT(0)
#define INT_OVERFLOW	BIT(8)

static volatile uint32_t ticks, wraps;
static uint32_t period = 1;

static uint64_t cnt_now(void)
{
	uint32_t flags = irq_save();
	uint32_t lo = readl(STTMR), hi = wraps;
	if (readl(STMINTSTAT) & INT_OVERFLOW) {
		lo = readl(STTMR);
		hi++;
	}
	irq_restore(flags);
	return (uint64_t)hi << 32 | lo;
}

uint64_t timer_count(void) { return cnt_now(); }
uint32_t timer_hz(void) { return SLEEP_CLK_HZ; }
uint32_t timer_ticks(void) { return ticks; }
uint32_t time_ms(void) { return (uint32_t)(cnt_now() * 1000 / SLEEP_CLK_HZ); }
uint32_t time_us(void) { return (uint32_t)(cnt_now() * 1000000 / SLEEP_CLK_HZ); }

/* Cycles from the debug unit's counter, which runs whenever the core
 * does. */
void delay_cycles(uint32_t n)
{
	uint32_t start = readl(DWT_CYCCNT);
	while (readl(DWT_CYCCNT) - start < n)
		;
}

void delay_us(uint32_t us)
{
	while (us > 1000000) {
		delay_cycles(CORE_HZ);
		us -= 1000000;
	}
	delay_cycles(us * (CORE_HZ / 1000000));
}

void delay_ms(uint32_t ms)
{
	while (ms--)
		delay_us(1000);
}

static uint32_t tick_count(uint32_t ms)
{
	uint64_t n = (uint64_t)(ms ? ms : 1) * SLEEP_CLK_HZ / 1000;
	return n < 2 ? 2 : n > 0x7fffffffu ? 0x7fffffffu : (uint32_t)n;
}

/* The compare register takes a distance from now and turns it into a
 * count; a write the timer did not take is tried again. */
static void compare_in(uint32_t n)
{
	uint32_t flags = irq_save();
	writel(readl(STCFG) & ~STCFG_COMPARE_A, STCFG);
	for (int tries = 0; tries < 4; tries++) {
		uint32_t want = readl(STTMR) + n;
		writel(n, SCMPR(0));
		uint32_t got = readl(SCMPR(0));
		if ((int32_t)(got - want) >= 0 && (int32_t)(got - want) <= 10)
			break;
	}
	writel(INT_COMPAREA, STMINTCLR);
	writel(readl(STCFG) | STCFG_COMPARE_A, STCFG);
	irq_restore(flags);
}

void timer_set_period(uint32_t ms) { period = ms ? ms : 1; }

void timer_next(uint32_t ms) { compare_in(tick_count(ms)); }

static void stimer_irq(void)
{
	uint32_t st = readl(STMINTSTAT);
	if (st & INT_OVERFLOW) {
		writel(INT_OVERFLOW, STMINTCLR);
		wraps++;
	}
}

static void tick_irq(void)
{
	if (!(readl(STMINTSTAT) & INT_COMPAREA))
		return;
	ticks++;
	compare_in(tick_count(period));
	prof_sample();
}

void timer_resume(void) {}

void timer_init(void)
{
	writel(readl(DEMCR) | BIT(24), DEMCR);
	writel(readl(DWT_CTRL) | 1, DWT_CTRL);
	/* The boot loader may have left it on another clock: start over
	 * from 0 on the crystal. */
	writel(STCFG_CLEAR | STCFG_FREEZE, STCFG);
	writel(STCFG_XTAL, STCFG);
	writel(~0u, STMINTCLR);
	writel(INT_COMPAREA | INT_OVERFLOW, STMINTEN);
	irq_attach(IRQ_STIMER, stimer_irq);
	irq_attach(IRQ_STIMER_CMPR(0), tick_irq);
	timer_next(1);
}

static uint32_t base, last_s;
static bool set;

bool rtc_valid(void) { return set; }

uint32_t rtc_read(void)
{
	uint32_t now = (uint32_t)(cnt_now() / SLEEP_CLK_HZ);
	last_s = now;
	return base + now;
}

void rtc_write(uint32_t secs)
{
	base = secs - (uint32_t)(cnt_now() / SLEEP_CLK_HZ);
	set = true;
}
