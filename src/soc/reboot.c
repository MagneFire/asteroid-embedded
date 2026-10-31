/* Warm reset through PS_HOLD. The IMEM download-mode cookies are cleared
 * first: a stale cookie makes the boot ROM stop in Qualcomm download mode
 * after any reset instead of running the bootloader. If PS_HOLD does not
 * take the chip down, the watchdog bite is the second attempt. */
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

/* The restart reason in IMEM tells the bootloader what to do after the
 * reset: this value makes it stay in fastboot. */
#define RESTART_FASTBOOT	0x77665500u

bool reboot_to_bootloader;

void reboot(void)
{
	settings_flush();
	irq_disable();
	dload_clear();
	if (reboot_to_bootloader)
		writel(RESTART_FASTBOOT, IMEM_RESTART_REASON);
	scm_wdog_debug_disable();
	dcache_clean_invalidate_all();
	writel(0, MPM2_PS_HOLD);
	delay_ms(1000);
	wdt_arm(50, 100, NULL);
	for (;;)
		wfi();
}
