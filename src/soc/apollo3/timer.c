/* Time is the system timer counting the 32 kHz crystal, which runs on in
 * deep sleep; its overflows, every 36 hours, are counted in its
 * interrupt. Its first compare gives the tick that wakes the main loop.
 * Short delays count core cycles instead, for their resolution. The wall
 * clock counts on from what was last set, like a clock that keeps no
 * time of its own: the phone sets it on connecting. Only a power-on
 * resets the timer's four spare registers, and no SRAM comes through
 * the boot loader, so they hold what the clock goes on from after a
 * reset: from the counter if the reset left it running, else from the
 * last time read. */
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
#define STCFG_MODE	(STCFG_FREEZE | STCFG_CLEAR | 0xfu)
#define RESET_COLD	(BIT(1) | BIT(2))
#define SNVR(n)		(CTIMER_BASE + 0x1f0 + 4 * (n))
#define NV_BASE		SNVR(0)
#define NV_WALL		SNVR(1)
#define NV_WRAPS	SNVR(2)
#define NV_SUM		SNVR(3)
#define NV_MAGIC	0x52544331u	/* "RTC1" */
#define AP_TAG		0x41503031u	/* "AP01" */
/* Longer than any sleep, shorter than the counter's wrap. */
#define KEPT_GAP_S	(30u * 3600u)

static volatile uint32_t ticks, wraps;
static uint32_t period = 1;
static const char *origin = "not set";

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

uint32_t cpu_hz = CORE_HZ;

/* Cycles from the debug unit's counter, which runs whenever the core
 * does, at its clock. */
void delay_cycles(uint32_t n)
{
	uint32_t start = readl(DWT_CYCCNT);
	while (readl(DWT_CYCCNT) - start < n)
		;
}

void delay_us(uint32_t us)
{
	while (us > 1000000) {
		delay_cycles(cpu_hz);
		us -= 1000000;
	}
	delay_cycles(us * (cpu_hz / 1000000));
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

void timer_wake_within(uint32_t ms)
{
	if (ms == UINT32_MAX)
		return;
	uint32_t n = tick_count(ms), flags = irq_save();
	if ((int32_t)(readl(SCMPR(0)) - readl(STTMR)) > (int32_t)n)
		compare_in(n);
	irq_restore(flags);
}

static void stimer_irq(void)
{
	uint32_t st = readl(STMINTSTAT);
	if (st & INT_OVERFLOW) {
		writel(INT_OVERFLOW, STMINTCLR);
		writel(++wraps, NV_WRAPS);
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

static uint32_t base;
static bool set, from_ap;

/* Not 0 for a base of 0, which is how a power-on leaves the registers. */
static uint32_t nv_sum(uint32_t b) { return b * 2654435761u + NV_MAGIC; }

/* The wall time and the wraps change all the time, so the sum leaves
 * them out; AP_TAG is in it while the time is the AP's. */
static void keep_base(void)
{
	writel(base, NV_BASE);
	writel(nv_sum(base) ^ (from_ap ? AP_TAG : 0), NV_SUM);
}

/* The wall clock from before a warm reset: 0 when there is none. */
static uint32_t kept_wall(void)
{
	origin = "not set";
	from_ap = false;
	uint32_t kept = readl(NV_BASE), tag = readl(NV_SUM) ^ nv_sum(kept);
	if ((tag && tag != AP_TAG) || (reset_status() & RESET_COLD))
		return 0;
	uint32_t wall = readl(NV_WALL);
	from_ap = tag == AP_TAG;
	origin = "kept, last read";
	if ((readl(STCFG) & STCFG_MODE) == STCFG_XTAL) {
		uint32_t lo = readl(STTMR), hi = readl(NV_WRAPS);
		if (readl(STMINTSTAT) & INT_OVERFLOW) {
			lo = readl(STTMR);
			hi++;
		}
		uint32_t t = kept + (uint32_t)(((uint64_t)hi << 32 | lo) / SLEEP_CLK_HZ);
		if (t - wall < KEPT_GAP_S) {
			wall = t;
			origin = "kept, counter";
		}
	}
	return wall;
}

void timer_init(void)
{
	writel(readl(DEMCR) | BIT(24), DEMCR);
	writel(readl(DWT_CTRL) | 1, DWT_CTRL);
	uint32_t wall = kept_wall();
	/* The boot loader may have left it on another clock: start over
	 * from 0 on the crystal. */
	writel(STCFG_CLEAR | STCFG_FREEZE, STCFG);
	writel(STCFG_XTAL, STCFG);
	writel(~0u, STMINTCLR);
	wraps = 0;
	writel(0, NV_WRAPS);
	base = wall;
	set = wall != 0;
	writel(wall, NV_WALL);
	if (set) {
		keep_base();
	} else {
		writel(0, NV_BASE);
		writel(0, NV_SUM);
	}
	writel(INT_COMPAREA | INT_OVERFLOW, STMINTEN);
	irq_attach(IRQ_STIMER, stimer_irq);
	irq_attach(IRQ_STIMER_CMPR(0), tick_irq);
	timer_next(1);
}

const char *rtc_origin(void) { return origin; }

bool rtc_valid(void) { return set; }

uint32_t rtc_read(void)
{
	uint32_t wall = base + (uint32_t)(cnt_now() / SLEEP_CLK_HZ);
	if (set)
		writel(wall, NV_WALL);
	return wall;
}

void rtc_write(uint32_t secs)
{
	base = secs - (uint32_t)(cnt_now() / SLEEP_CLK_HZ);
	set = true;
	from_ap = false;
	origin = "set";
	writel(secs, NV_WALL);
	keep_base();
}

void rtc_write_from_ap(uint32_t secs)
{
	rtc_write(secs);
	from_ap = true;
	origin = "set by the AP";
	keep_base();
}

bool rtc_from_ap(void) { return from_ap; }
