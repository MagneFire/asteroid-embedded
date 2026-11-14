/* What main does for this SoC. */
#include "asteroid.h"

static int scm;

void soc_init(void)
{
	boot_stage(BOOT_STAGE("chip"));
	chip_init();
	boot_stage(BOOT_STAGE("clk "));
	cpu_clock_init();
	boot_stage(BOOT_STAGE("mmu "));
	mmu_init();
	boot_stage(BOOT_STAGE("irq "));
	irq_ctrl_init();
	boot_stage(BOOT_STAGE("tmr "));
	timer_init();
	boot_stage(BOOT_STAGE("dld "));
	dload_clear();
	boot_stage(BOOT_STAGE("scm "));
	scm = scm_wdog_debug_disable();
}

void soc_board_init(void)
{
	spmi_init();
	pmic_init();
	if (rpm_init()) {
		rpm_bus_votes();
		rpm_sleep_votes();
		rpm_unused_rails_off();
	}
	log_printf("spmi arbiter %08lx, %s, scm %d\n", spmi_version(), boot_reason(), scm);
}
