/* Warm reset through PS_HOLD. The IMEM download-mode cookies are cleared
 * first: a stale cookie makes the boot ROM stop in Qualcomm download mode
 * after any reset instead of running the bootloader. The APQ8009W keeps
 * that flag in the secure world only, which scm_wdog_debug_disable
 * clears. If PS_HOLD does not take the chip down, the watchdog bite is
 * the second attempt. */
#include "asteroid.h"

void dload_clear(void)
{
	if (!chip->imem_dload) {
		writel(0, IMEM_RESTART_REASON);
		dsb();
		return;
	}
	writel(0, IMEM_DLOAD_MAGIC);
	writel(0, IMEM_DLOAD_MAGIC + 4);
	writel(0, IMEM_EDLOAD_MAGIC);
	writel(0, IMEM_EDLOAD_MAGIC + 4);
	writel(0, IMEM_EDLOAD_MAGIC + 8);
	writel(0, IMEM_RESTART_REASON);
	dsb();
}

/* The restart reason in IMEM tells the bootloader what to do after the
 * reset: this value makes it stay in fastboot. The APQ8009W's bootloader
 * also reads the PMIC's spare PON bits 7:2, so both are written. */
#define RESTART_FASTBOOT	0x77665500u
#define PON_SOFT_RB_SPARE	0x088f
#define PON_RESTART_FASTBOOT	(2u << 2)

bool reboot_to_bootloader;

static void __attribute__((noreturn)) reset_now(void)
{
	irq_disable();
	dload_clear();
	if (reboot_to_bootloader)
		writel(RESTART_FASTBOOT, IMEM_RESTART_REASON);
	if (chip->pon_restart_reason)
		pmic_write(PON_SOFT_RB_SPARE, (pmic_read(PON_SOFT_RB_SPARE) & 3) | (reboot_to_bootloader ? PON_RESTART_FASTBOOT : 0));
	scm_wdog_debug_disable();
	dcache_clean_invalidate_all();
	writel(0, MPM2_PS_HOLD);
	delay_ms(1000);
	wdt_arm(50, 100, NULL);
	for (;;)
		wfi();
}

void reboot(void)
{
	settings_flush();
	reset_now();
}

/* Hardware the firmware depends on is gone: leave a record for the next
 * boot and reset at once, as anything more could hang on the same bus. */
void reboot_fatal(const char *what)
{
	uint32_t lr = (uint32_t)(uintptr_t)__builtin_return_address(0);
	fault_record(what, lr, lr, (uint32_t)(uintptr_t)__builtin_frame_address(0), NULL);
	reset_now();
}
