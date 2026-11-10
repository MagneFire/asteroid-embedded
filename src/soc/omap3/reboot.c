/* Reset and power-off. The bootloader reads a word in the scratchpad
 * after a reset: 'B', 'M' and a command, 'b' to stay in its fastboot,
 * nothing to start the installed system. The reset is the PRCM's
 * global software reset. Power-off is the PMIC's doing: a rising edge
 * on its watchdog input, a pin that is otherwise left unconnected. */
#include "asteroid.h"

#define BOOT_MODE(cmd)		((uint32_t)'B' << 24 | (uint32_t)'M' << 16 | (cmd))
#define RST_DPLL3		BIT(2)
#define PAD_WDI			0x142
#define GPIO_WDI		143

bool reboot_to_bootloader;
static bool power_off;

void power_off_arm(void) { power_off = true; }

static void __attribute__((noreturn)) reset_now(void)
{
	irq_disable();
	dcache_clean_invalidate_all();
	if (power_off) {
		gpio_set(GPIO_WDI, 0);
		gpio_config(GPIO_WDI, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
		pad_config(PAD_WDI, PAD_MODE_GPIO);
		/* With a cable in the chip comes straight back up otherwise. */
		if (vbus_present())
			delay_ms(500);
		gpio_set(GPIO_WDI, 1);
		delay_ms(1000);
	}
	writel(BOOT_MODE(reboot_to_bootloader ? 'b' : 0), CONTROL_SCRATCHPAD_BOOT);
	writel(readl(PRM_RSTCTRL) | RST_DPLL3, PRM_RSTCTRL);
	delay_ms(1000);
	wdt_arm(0, 100, NULL);
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
