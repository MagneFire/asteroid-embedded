/* The OMAP interrupt controller: 96 level-sensitive lines, all at one
 * priority and all routed to IRQ. */
#include "asteroid.h"

#define INTC_SYSCONFIG		(INTC_BASE + 0x10)
#define INTC_SYSSTATUS		(INTC_BASE + 0x14)
#define INTC_SIR_IRQ		(INTC_BASE + 0x40)
#define INTC_CONTROL		(INTC_BASE + 0x48)
#define INTC_MIR_CLEAR(n)	(INTC_BASE + 0x88 + 0x20 * (n))
#define INTC_MIR_SET(n)		(INTC_BASE + 0x8c + 0x20 * (n))
#define INTC_ILR(m)		(INTC_BASE + 0x100 + 4 * (m))
#define NEWIRQAGR		BIT(0)
#define SPURIOUS		0xffffff80u

static void (*handlers[IRQ_COUNT])(void);
static uint32_t spurious;
const uint32_t *irq_frame;

void irq_ctrl_init(void)
{
	writel(BIT(1), INTC_SYSCONFIG);
	while (!(readl(INTC_SYSSTATUS) & 1))
		;
	for (unsigned i = 0; i < IRQ_COUNT / 32; i++)
		writel(~0u, INTC_MIR_SET(i));
	for (unsigned i = 0; i < IRQ_COUNT; i++)
		writel(0, INTC_ILR(i));
	writel(NEWIRQAGR, INTC_CONTROL);
}

void irq_attach(unsigned id, void (*fn)(void))
{
	if (id >= IRQ_COUNT)
		return;
	handlers[id] = fn;
	writel(BIT(id % 32), INTC_MIR_CLEAR(id / 32));
}

/* Every line is level-sensitive here. */
void irq_set_edge(unsigned id) {}

void irq_mask(unsigned id)
{
	if (id < IRQ_COUNT)
		writel(BIT(id % 32), INTC_MIR_SET(id / 32));
}

void irq_unmask(unsigned id)
{
	if (id < IRQ_COUNT)
		writel(BIT(id % 32), INTC_MIR_CLEAR(id / 32));
}

void irq_detach(unsigned id)
{
	if (id >= IRQ_COUNT)
		return;
	writel(BIT(id % 32), INTC_MIR_SET(id / 32));
	handlers[id] = 0;
}

static volatile unsigned depth;

bool in_irq(void) { return depth != 0; }

/* frame: {r0-r3, r12, pc} of the interrupted code, for handlers that
 * report where the main loop was. */
void irq_handler(const uint32_t *frame)
{
	irq_frame = frame;
	uint32_t sir = readl(INTC_SIR_IRQ);
	if (sir & SPURIOUS)
		spurious++;
	else {
		unsigned id = sir & 0x7f;
		depth++;
		if (id < IRQ_COUNT && handlers[id])
			handlers[id]();
		else
			writel(BIT(id % 32), INTC_MIR_SET(id / 32));
		depth--;
	}
	writel(NEWIRQAGR, INTC_CONTROL);
	dsb();
}
