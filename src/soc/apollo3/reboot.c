/* Reset, and why the chip came up. The reset is the software POI, which
 * resets the core and the peripherals and keeps SRAM, so the log goes
 * across; the boot loader then starts the image again. There is no power
 * off of its own: the battery stays connected, and "off" is the deepest
 * sleep with the panel and the radio off and only the power key left to
 * wake it, which then resets. */
#include "asteroid.h"
#include "bt/bt.h"
#include "display/display.h"

#define SWPOI_KEY	0x1bu

bool reboot_to_bootloader;
static bool power_off;
static char reason[64] = "unknown";
static uint32_t status;

void power_off_arm(void) { power_off = true; }

const char *boot_reason(void)
{
	static bool read;
	if (!read) {
		static const char *const bits[] = { "external pin", "power-on", "brown-out", "software POR",
						    "software POI", "debugger", "watchdog" };
		uint32_t st = status = readl(RSTGEN_STAT);
		writel(0, RSTGEN_STAT);
		size_t len = 0;
		for (unsigned i = 0; i < ARRAY_SIZE(bits) && len < sizeof reason - 1; i++)
			if (st & BIT(i)) {
				int n = snprintf(reason + len, sizeof reason - len, "%s%s", len ? ", " : "", bits[i]);
				if (n > 0)
					len = len + (size_t)n < sizeof reason ? len + (size_t)n : sizeof reason - 1;
			}
		if (!len)
			snprintf(reason, sizeof reason, "reset status %08lx", st);
		read = true;
	}
	return reason;
}

/* With the interrupts masked, the key's pending line still ends the
 * WFI. */
static void sleep_off(void)
{
	display_sleep();
	bt_stop();
	irq_disable();
	wdt_stop();
	writel(~0u, NVIC_ICER(0));
	unsigned key = board->key_gpio[0];
	for (unsigned n = 0; n < GPIO_COUNT; n++)
		gpio_irq_enable(n, key && n == key);
	if (key) {
		gpio_irq_ack(key);
		writel(BIT(IRQ_GPIO), NVIC_ICPR(0));
		writel(BIT(IRQ_GPIO), NVIC_ISER(0));
	}
	writel(readl(SCB_SCR) | BIT(2), SCB_SCR);
	while (!key || !(readl(NVIC_ISPR(0)) & BIT(IRQ_GPIO)))
		wfi();
}

uint32_t reset_status(void)
{
	boot_reason();
	return status;
}

static void __attribute__((noreturn)) reset_now(void)
{
	if (power_off)
		sleep_off();
	irq_disable();
	writel(SWPOI_KEY, RSTGEN_SWPOI);
	for (;;)
		;
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
