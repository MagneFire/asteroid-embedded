/* The NVIC: the chip's 32 lines, all at one priority, so a handler is
 * never interrupted by another. */
#include "asteroid.h"

static void (*handlers[IRQ_COUNT])(void);
static uint32_t stray;
volatile unsigned irq_last;
const uint32_t *irq_frame;

void irq_ctrl_init(void)
{
	writel(~0u, NVIC_ICER(0));
	writel(~0u, NVIC_ICPR(0));
	for (unsigned i = 0; i < IRQ_COUNT; i++)
		*(volatile uint8_t *)(uintptr_t)NVIC_IPR(i) = 0x80;
}

void irq_attach(unsigned id, void (*fn)(void))
{
	if (id >= IRQ_COUNT)
		return;
	handlers[id] = fn;
	writel(BIT(id), NVIC_ICPR(0));
	writel(BIT(id), NVIC_ISER(0));
}

/* The peripherals latch their own events; the NVIC needs nothing. */
void irq_set_edge(unsigned id) {}

void irq_mask(unsigned id)
{
	if (id < IRQ_COUNT)
		writel(BIT(id), NVIC_ICER(0));
}

void irq_unmask(unsigned id)
{
	if (id < IRQ_COUNT)
		writel(BIT(id), NVIC_ISER(0));
}

void irq_detach(unsigned id)
{
	if (id >= IRQ_COUNT)
		return;
	writel(BIT(id), NVIC_ICER(0));
	handlers[id] = 0;
}

bool in_irq(void)
{
	uint32_t ipsr;
	__asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
	return ipsr != 0;
}

/* exc is the exception number, 16 and up for the chip's lines; frame
 * the {r0-r3, r12, lr, pc, xpsr} the core stacked. */
void irq_handler(uint32_t exc, const uint32_t *frame)
{
	irq_frame = frame;
	unsigned id = exc - 16;
	if (exc >= 16)
		irq_last = id;
	if (exc >= 16 && id < IRQ_COUNT && handlers[id])
		handlers[id]();
	else if (exc >= 16 && id < IRQ_COUNT) {
		writel(BIT(id), NVIC_ICER(0));
		stray++;
	}
}
