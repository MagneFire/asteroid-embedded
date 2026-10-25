/* Warm reset through PS_HOLD. The IMEM download-mode cookies are cleared
 * first: a stale cookie makes the boot ROM stop in Qualcomm download mode
 * after any reset instead of running the bootloader. */
#include "asteroid.h"

void dload_clear(void)
{
	writel(0, IMEM_DLOAD_MAGIC);
	writel(0, IMEM_DLOAD_MAGIC + 4);
	writel(0, IMEM_EDLOAD_MAGIC);
	writel(0, IMEM_EDLOAD_MAGIC + 4);
	writel(0, IMEM_EDLOAD_MAGIC + 8);
	writel(0, IMEM_RESTART_REASON);
	dsb();
}

void reboot(void)
{
	irq_disable();
	dload_clear();
	wdt_stop();
	dcache_clean_invalidate_all();
	writel(0, MPM2_PS_HOLD);
	for (;;)
		wfi();
}
