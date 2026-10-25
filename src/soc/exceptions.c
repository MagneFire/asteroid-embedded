/* Any exception is fatal: the registers, the interrupted mode's lr/sp and
 * the top of its stack go into the persistent log as a record for the next
 * boot, then the chip is warm-reset. */
#include "asteroid.h"

static const char *const names[] = { "?", "undef", "svc", "pabt", "dabt", "fiq" };

static void stack_words(char *buf, size_t n, uint32_t sp)
{
	size_t len = 0;
	if (sp < (uint32_t)(uintptr_t)_start || sp >= 0x02f00000u)
		return;
	const uint32_t *w = (const uint32_t *)(uintptr_t)sp;
	for (int i = 0; i < 32 && len < n; i++)
		len += (size_t)snprintf(buf + len, n - len, "%s%08lx%s", (i % 8) ? " " : "stack: ",
					w[i], (i % 8 == 7) ? "\n" : "");
}

void fault_record(const char *kind, uint32_t pc, uint32_t lr, uint32_t sp, const uint32_t *regs)
{
	static char rec[1024];
	size_t len = 0;
	uint32_t dfar, dfsr, ifar, ifsr;
	__asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(dfar));
	__asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(dfsr));
	__asm__ volatile("mrc p15, 0, %0, c6, c0, 2" : "=r"(ifar));
	__asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(ifsr));
	len += (size_t)snprintf(rec + len, sizeof rec - len,
				"%s pc=%08lx lr=%08lx sp=%08lx dfar=%08lx dfsr=%03lx ifar=%08lx ifsr=%03lx t=%lu build=%s\n",
				kind, pc, lr, sp, dfar, dfsr & 0xfff, ifar, ifsr & 0xfff, time_ms(), build_id);
	if (regs)
		for (int i = 0; i < 13; i++)
			len += (size_t)snprintf(rec + len, sizeof rec - len, "r%d=%08lx%s", i, regs[i],
						(i % 4 == 3 || i == 12) ? "\n" : " ");
	stack_words(rec + len, sizeof rec - len, sp);
	log_puts("\n!!! ");
	log_puts(rec);
	log_record(rec);
}

void exception_handler(uint32_t type, uint32_t *frame)
{
	uint32_t pc = frame[13], spsr, cpsr, lr_x = 0, sp_x = 0;
	if (type > 5)
		type = 0;
	__asm__ volatile("mrs %0, spsr\n mrs %1, cpsr" : "=r"(spsr), "=r"(cpsr));
	if ((spsr & 0x1f) == 0x13)
		__asm__ volatile("cps #0x13\n mov %0, lr\n mov %1, sp\n msr cpsr_c, %2" : "=&r"(lr_x), "=&r"(sp_x) : "r"(cpsr) : "memory");
	else if ((spsr & 0x1f) == 0x12)
		__asm__ volatile("cps #0x12\n mov %0, lr\n mov %1, sp\n msr cpsr_c, %2" : "=&r"(lr_x), "=&r"(sp_x) : "r"(cpsr) : "memory");
	fault_record(names[type], pc, lr_x, sp_x, frame);
	reboot();
}

/* The watchdog barked: the main loop has not fed it for a while but
 * interrupts still run. The interrupted pc is where it is stuck. */
extern const uint32_t *irq_frame;

void wdt_bark_handler(void)
{
	uint32_t cpsr, lr_x = 0, sp_x = 0, pc = irq_frame ? irq_frame[5] : 0;
	__asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
	__asm__ volatile("cps #0x13\n mov %0, lr\n mov %1, sp\n msr cpsr_c, %2" : "=&r"(lr_x), "=&r"(sp_x) : "r"(cpsr) : "memory");
	fault_record("watchdog", pc, lr_x, sp_x, NULL);
	reboot();
}
