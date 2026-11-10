/* What main does for this SoC, and its memory map. */
#include "asteroid.h"

void soc_memory_map(void)
{
	mmu_map(DRAM_BASE, DRAM_END, MMU_RAM);
	mmu_map(RAMOOPS_BASE & ~0xfffffu, (RAMOOPS_BASE + RAMOOPS_SIZE + 0xfffffu) & ~0xfffffu, MMU_RAM_UNCACHED);
	/* L4 core with the display, wake-up and peripheral modules. */
	mmu_map(0x48000000, 0x48400000, MMU_DEVICE);
	mmu_map(0x49000000, 0x49100000, MMU_DEVICE);
}

void soc_init(void)
{
	cpu_clock_init();
	mmu_init();
	irq_ctrl_init();
	/* The 96 and 48 MHz clocks of the buses, which a kernel that ran
	 * before may have switched off at their source. */
	writel(readl(CM_CCR + CM_CLKEN_PLL) & ~CM_PWRDN_96M, CM_CCR + CM_CLKEN_PLL);
	clock_enable(CM_WKUP, WKUP_32KSYNC | WKUP_WDT2, true);
	timer_init();
	gpio_init();
}

void soc_board_init(void)
{
	/* The GPIOs on the SIM card's pins, one of them the sensor hub's
	 * reset, drive nothing until their cells are switched on, at 1.8 V. */
	writel((readl(CONTROL_PBIAS_LITE) & ~(PBIAS_3V_1 | PBIAS_FAST_0)) | PBIAS_ON_1 | PBIAS_ON_0 | PBIAS_3V_0, CONTROL_PBIAS_LITE);
	writel(readl(CONTROL_WKUP_CTRL) | WKUP_SIM_IO_ON, CONTROL_WKUP_CTRL);
	for (const struct board_pad *p = board->pads; p && p->off; p++)
		pad_config(p->off, p->value);
	/* The bootloader's fastboot is still attached: its PHY goes off
	 * here, so that the host has long seen it leave by the time USB
	 * starts. */
	if (board->usb_phy.resetn) {
		gpio_set(board->usb_phy.resetn, 0);
		gpio_set(board->usb_phy.cs, 0);
		gpio_set(board->usb_phy.csn, 1);
		gpio_config(board->usb_phy.resetn, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
		gpio_config(board->usb_phy.cs, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
		gpio_config(board->usb_phy.csn, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	}
	tps65912_init();
	log_printf("boot: %s\n", boot_reason());
}
