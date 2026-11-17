/* The Apollo3's wall clock across resets: timer.c against a played
 * system timer, its counter moved by hand. A reset is timer_init again,
 * with .kept as the last run left it. */
#include <stdio.h>
#include <string.h>
#include "prof.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define STCFG		(CTIMER_BASE + 0x140)
#define STTMR		(CTIMER_BASE + 0x144)
#define SCMPR0		(CTIMER_BASE + 0x150)
#define STMINTSTAT	(CTIMER_BASE + 0x304)
#define STMINTCLR	(CTIMER_BASE + 0x308)
#define HZ		SLEEP_CLK_HZ

static uint32_t stcfg, count, cmp, intstat, rst;
static void (*overflow_irq)(void);

uint32_t readl(uint32_t a)
{
	switch (a) {
	case STCFG: return stcfg;
	case STTMR: return count;
	case SCMPR0: return cmp;
	case STMINTSTAT: return intstat;
	default: return 0;
	}
}

void writel(uint32_t v, uint32_t a)
{
	if (a == STCFG) {
		stcfg = v & ~BIT(30);
		if (v & BIT(30))
			count = 0;
	} else if (a == SCMPR0)
		cmp = count + v;
	else if (a == STMINTCLR)
		intstat &= ~v;
}

void dsb(void) {}
void dmb(void) {}
uint32_t irq_save(void) { return 0; }
void irq_restore(uint32_t flags) {}
void irq_attach(unsigned id, void (*fn)(void))
{
	if (id == IRQ_STIMER)
		overflow_irq = fn;
}
void prof_sample(void) {}
uint32_t reset_status(void) { return rst; }

/* What a reset does to the timer: kept running, or back to its reset
 * state. */
static void reset(bool timer_kept, uint32_t status)
{
	if (!timer_kept) {
		stcfg = BIT(31);
		count = 0;
	}
	rst = status;
	timer_init();
}

int main(void)
{
	/* Nothing kept on the first start. */
	timer_init();
	CHECK(!rtc_valid() && !strcmp(rtc_origin(), "not set"));

	count = 7 * HZ + 100;
	rtc_write(1700000000);
	CHECK(rtc_valid() && rtc_read() == 1700000000 && !strcmp(rtc_origin(), "set"));
	count += 10 * HZ;
	CHECK(rtc_read() == 1700000010);

	/* The counter ran on through the reset. */
	count += 5 * HZ;
	reset(true, BIT(4));
	CHECK(rtc_valid() && !strcmp(rtc_origin(), "kept, counter"));
	CHECK(rtc_read() == 1700000015);

	/* An overflow counted before the reset, and one pending at it. */
	const uint32_t wrap_s = (uint32_t)((1ull << 32) / HZ);
	count = 2 * HZ;
	intstat = BIT(8);
	overflow_irq();
	CHECK(rtc_read() == 1700000015 + wrap_s + 2);
	count = 0xffffffffu - HZ;
	rtc_read();
	count = 3 * HZ;
	intstat = BIT(8);
	reset(true, BIT(4));
	CHECK(!strcmp(rtc_origin(), "kept, counter") && rtc_read() == 1700000015 + 2 * wrap_s + 3);

	/* The reset cleared the counter: on from the last time read. */
	count = 30 * HZ;
	uint32_t last = rtc_read();
	count += 20 * HZ;
	reset(false, BIT(4));
	CHECK(rtc_valid() && !strcmp(rtc_origin(), "kept, last read") && rtc_read() == last);

	/* A counter that went back is not the one from before. */
	count = 100 * HZ;
	last = rtc_read();
	stcfg = 3;
	count = 50 * HZ;
	reset(true, BIT(5));
	CHECK(rtc_valid() && rtc_read() == last);

	/* Power-on and brown-out start without a time. */
	reset(true, BIT(1));
	CHECK(!rtc_valid() && !strcmp(rtc_origin(), "not set"));
	reset(true, BIT(4));
	CHECK(!rtc_valid());

	if (!failures)
		printf("apollo3_timer_test: ok\n");
	return failures != 0;
}
