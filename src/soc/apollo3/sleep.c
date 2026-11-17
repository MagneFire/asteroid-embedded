/* Idle. A light idle is sleep with the core's clock stopped; a deep one
 * stops the 48 MHz clock too, with SRAM kept, and any enabled interrupt
 * ends it. The system timer and the pads run on the 32 kHz side, so time
 * goes on and the wake-up lines need nothing of their own. */
#include "asteroid.h"
#include "sched.h"

#define SCR_SLEEPDEEP	BIT(2)
#define MCUCTRL_SIMOBUCK1	(MCUCTRL_BASE + 0x350)
#define CORE_ACT_TRIM		0x3ffu
#define MEM_ACT_TRIM_SHIFT	16
#define MEM_ACT_TRIM		0x3fu
#define SIMOBUCKON		BIT(0)

static uint32_t deep_sleeps, kept_light;

bool cpu_idle_init(void)
{
	/* SRAM stays powered in deep sleep; the flash too, as the image
	 * runs from it. */
	writel(0, PWRCTRL_MEMPWDINSLEEP);
	return true;
}

/* Erratum ERR028: the buck's rails have to enter deep sleep 60 mV above
 * their active level, or a short one can end in a brown-out. */
static void buck_lift(void)
{
	if (!(readl(PWRCTRL_SUPPLYSTATUS) & SIMOBUCKON))
		return;
	uint32_t v = readl(MCUCTRL_SIMOBUCK1);
	uint32_t core = (v & CORE_ACT_TRIM) + 24, mem = (v >> MEM_ACT_TRIM_SHIFT & MEM_ACT_TRIM) + 9;
	if (core > CORE_ACT_TRIM)
		core = CORE_ACT_TRIM;
	if (mem > MEM_ACT_TRIM)
		mem = MEM_ACT_TRIM;
	writel((v & ~(CORE_ACT_TRIM | MEM_ACT_TRIM << MEM_ACT_TRIM_SHIFT)) | core | mem << MEM_ACT_TRIM_SHIFT,
	       MCUCTRL_SIMOBUCK1);
	delay_us(20);
	writel(v, MCUCTRL_SIMOBUCK1);
}

/* With interrupts masked, an event posted before this point is seen
 * here and one posted after it ends the wait: wfi returns for a masked
 * interrupt, and it is taken on unmasking. */
void cpu_idle(bool deep)
{
	if (deep && swd_wanted()) {
		deep = false;
		kept_light++;
	}
	__asm__ volatile("cpsid i" ::: "memory");
	if (!sched_pending()) {
		if (deep) {
			buck_lift();
			writel(readl(SCB_SCR) | SCR_SLEEPDEEP, SCB_SCR);
		}
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
	*cluster_collapses = 0;
	*bailouts = kept_light;
}
