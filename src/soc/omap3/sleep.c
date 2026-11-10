/* Idle: wait for an interrupt with the core's clock stopped. */
#include "asteroid.h"
#include "sched.h"

bool cpu_idle_init(void) { return true; }

/* With interrupts masked, an event posted before this point is seen
 * here and one posted after it ends the wait: wfi returns for a masked
 * interrupt, and it is taken on unmasking. */
void cpu_idle(bool deep)
{
	__asm__ volatile("cpsid i" ::: "memory");
	if (!sched_pending())
		wfi();
	__asm__ volatile("cpsie i" ::: "memory");
}

void wake_pin(unsigned pin, bool on, bool high) {}

void cpu_idle_stats(uint32_t *collapses, uint32_t *cluster_collapses, uint32_t *bailouts)
{
	*collapses = *cluster_collapses = *bailouts = 0;
}
