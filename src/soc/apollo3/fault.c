/* Any fault is fatal: the registers, the fault status and the top of the
 * stack go into the persistent log as a record for the next boot, then
 * the chip is reset. */
#include "asteroid.h"

static void stack_words(char *buf, size_t n, uint32_t sp)
{
	size_t len = 0;
	if (sp < SRAM_BASE || sp + 128 > DRAM0_END)
		return;
	const uint32_t *w = (const uint32_t *)(uintptr_t)sp;
	for (int i = 0; i < 32 && len < n; i++)
		len += (size_t)snprintf(buf + len, n - len, "%s%08lx%s", (i % 8) ? " " : "stack: ", w[i],
					(i % 8 == 7) ? "\n" : "");
}

void fault_record(const char *kind, uint32_t pc, uint32_t lr, uint32_t sp, const uint32_t *regs)
{
	static char rec[1024];
	size_t len = 0;
	len += (size_t)snprintf(rec + len, sizeof rec - len,
				"%s pc=%08lx lr=%08lx sp=%08lx cfsr=%08lx hfsr=%08lx mmfar=%08lx bfar=%08lx t=%lu build=%s\n",
				kind, pc, lr, sp, readl(SCB_CFSR), readl(SCB_HFSR), readl(SCB_MMFAR), readl(SCB_BFAR),
				time_ms(), build_id);
	if (regs)
		for (int i = 0; i < 13; i++)
			len += (size_t)snprintf(rec + len, sizeof rec - len, "r%d=%08lx%s", i, regs[i],
						(i % 4 == 3 || i == 12) ? "\n" : " ");
	stack_words(rec + len, sizeof rec - len, sp);
	log_puts("\n!!! ");
	log_puts(rec);
	log_record(rec);
}

static const char *const names[] = { "?", "reset", "nmi", "hardfault", "memmanage", "busfault", "usagefault" };

/* frame: the core's {r0-r3, r12, lr, pc, xpsr}; high: r4-r11. */
void fault_handler(uint32_t exc, const uint32_t *frame, const uint32_t *high)
{
	uint32_t regs[13];
	bool sane = (uint32_t)(uintptr_t)frame >= SRAM_BASE && (uint32_t)(uintptr_t)frame + 32 <= DRAM0_END;
	for (int i = 0; i < 4; i++)
		regs[i] = sane ? frame[i] : 0;
	for (int i = 0; i < 8; i++)
		regs[4 + i] = high[i];
	regs[12] = sane ? frame[4] : 0;
	const char *kind = exc < ARRAY_SIZE(names) ? names[exc] : exc == 11 ? "svc" : "exception";
	fault_record(kind, sane ? frame[6] : 0, sane ? frame[5] : 0, (uint32_t)(uintptr_t)frame + 32, regs);
	reboot();
}

extern const uint32_t *irq_frame;

/* The watchdog barked: the main loop has not fed it for a while but
 * interrupts still run. The interrupted pc is where it is stuck. */
void wdt_bark_handler(void)
{
	uint32_t pc = irq_frame ? irq_frame[6] : 0, lr = irq_frame ? irq_frame[5] : 0;
	fault_record("watchdog", pc, lr, (uint32_t)(uintptr_t)irq_frame + 32, NULL);
	reboot();
}
