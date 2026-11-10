/* Idle. A light idle waits for an interrupt with the core's clock
 * stopped. A deep one also lets the processor's power domain drop to
 * retention, which keeps its logic and caches at a lower voltage and
 * comes back where it left off, on any interrupt that is let through.
 * The NEON unit's domain follows it. The rest of the chip is kept up,
 * so the clocks time is counted from keep running. */
#include "asteroid.h"
#include "sched.h"

#define PM_PWSTCTRL_MPU		(PRM_MPU + 0xe0)
#define PM_PREPWSTST_MPU	(PRM_MPU + 0xe8)
#define CM_AUTOIDLE_PLL_MPU	(CM_MPU + 0x34)
/* Memory on while running, memory and logic kept in retention. */
#define MPU_KEEP		(3u << 16 | BIT(8) | BIT(2))
#define MPU_ON			3u
#define MPU_RETENTION		1u
#define CLKSTCTRL_AUTO		3u
#define CLKSTCTRL_STAY		0u
#define PM_PWSTCTRL_NEON	(PRM_BASE + 0x13e0)
#define NEON_KEEP		BIT(2)
#define CONTROL_MEM_RTA		(CONTROL_BASE + 0x40c)
#define CLKSRC_AUTO_OFF		(3u << 3)	/* the crystal stopped by itself */

static uint32_t retentions;

bool cpu_idle_init(void)
{
	writel(MPU_KEEP | MPU_ON, PM_PWSTCTRL_MPU);
	writel(NEON_KEEP | MPU_ON, PM_PWSTCTRL_NEON);
	writel(CLKSTCTRL_STAY, CM_CORE + CM_CLKSTCTRL);
	writel(CLKSTCTRL_STAY, CM_PER + CM_CLKSTCTRL);
	writel(readl(PRM_CLKSRC_CTRL) & ~CLKSRC_AUTO_OFF, PRM_CLKSRC_CTRL);
	/* The memories' own retention between accesses loses data on this
	 * chip. */
	writel(0, CONTROL_MEM_RTA);
	writel(CLKSTCTRL_AUTO, CM_MPU + CM_CLKSTCTRL);
	writel(1, CM_AUTOIDLE_PLL_MPU);
	return true;
}

/* With interrupts masked, an event posted before this point is seen
 * here and one posted after it ends the wait: wfi returns for a masked
 * interrupt, and it is taken on unmasking. */
void cpu_idle(bool deep)
{
	__asm__ volatile("cpsid i" ::: "memory");
	if (!sched_pending()) {
		if (deep) {
			writel(0, PM_PREPWSTST_MPU);
			writel(MPU_KEEP | MPU_RETENTION, PM_PWSTCTRL_MPU);
			writel(NEON_KEEP | MPU_RETENTION, PM_PWSTCTRL_NEON);
		}
		wfi();
		if (deep) {
			writel(MPU_KEEP | MPU_ON, PM_PWSTCTRL_MPU);
			writel(NEON_KEEP | MPU_ON, PM_PWSTCTRL_NEON);
			if ((readl(PM_PREPWSTST_MPU) & 3) == MPU_RETENTION)
				retentions++;
		}
	}
	__asm__ volatile("cpsie i" ::: "memory");
}

/* The pad's wake-up, for the day the rest of the chip sleeps too. */
void wake_pin(unsigned pin, bool on, bool high)
{
	if (!pin || pin >= board->wake_pad_count || !board->wake_pads[pin])
		return;
	uint32_t pad = pad_read(board->wake_pads[pin]);
	pad_config(board->wake_pads[pin], on ? pad | PAD_WAKEUP : pad & ~PAD_WAKEUP);
}

void cpu_idle_stats(uint32_t *collapses, uint32_t *cluster_collapses, uint32_t *bailouts)
{
	*collapses = retentions;
	*cluster_collapses = *bailouts = 0;
}
