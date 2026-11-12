/* What main does for this SoC. */
#include "asteroid.h"

static int scm;

void soc_init(void)
{
	chip_init();
	cpu_clock_init();
	mmu_init();
	irq_ctrl_init();
	timer_init();
	dload_clear();
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
