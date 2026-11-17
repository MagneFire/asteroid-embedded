/* Idle. A light idle is sleep with the core's clock stopped; a deep one
 * stops the 48 MHz clock too, with SRAM kept, and any enabled interrupt
 * ends it. The system timer and the pads run on the 32 kHz side, so time
 * goes on and the wake-up lines need nothing of their own. */
#include "asteroid.h"
#include "sched.h"

#define SCR_SLEEPDEEP	BIT(2)
#define DHCSR		0xe000edf0u
#define C_DEBUGEN	BIT(0)

static uint32_t deep_sleeps;

bool cpu_idle_init(void)
{
	/* SRAM stays powered in deep sleep; the flash too, as the image
	 * runs from it. */
	writel(0, PWRCTRL_MEMPWDINSLEEP);
	return true;
}

/* With interrupts masked, an event posted before this point is seen
 * here and one posted after it ends the wait: wfi returns for a masked
 * interrupt, and it is taken on unmasking. */
void cpu_idle(bool deep)
{
	/* A debug access during deep sleep breaks the AP's SWD link until
	 * it reconnects, so with debug on the core only sleeps lightly, as
	 * the vendor's firmware does. */
	if (readl(DHCSR) & C_DEBUGEN)
		deep = false;
	__asm__ volatile("cpsid i" ::: "memory");
	if (!sched_pending()) {
		if (deep)
			writel(readl(SCB_SCR) | SCR_SLEEPDEEP, SCB_SCR);
		dsb();
		wfi();
		if (deep) {
			writel(readl(SCB_SCR) & ~SCR_SLEEPDEEP, SCB_SCR);
			deep_sleeps++;
		}
	}
	__asm__ volatile("cpsie i" ::: "memory");
}

void cpu_idle_limit(enum idle_level level) {}
void wake_pin(unsigned pin, bool on, bool high) {}

void cpu_idle_stats(uint32_t *collapses, uint32_t *cluster_collapses, uint32_t *bailouts)
{
	*collapses = deep_sleeps;
	*cluster_collapses = *bailouts = 0;
}
