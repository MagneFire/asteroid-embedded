/* GICv2 distributor and CPU interface, non-secure view: the secure world
 * owns the group assignment, so only Group 1 is visible here and every
 * interrupt the firmware uses must already be non-secure (they are: the
 * Android kernel used the same ones). */
#include "asteroid.h"

#define GICD_CTLR		(GICD_BASE + 0x000)
#define GICD_TYPER		(GICD_BASE + 0x004)
#define GICD_ISENABLER(n)	(GICD_BASE + 0x100 + 4 * (n))
#define GICD_ICENABLER(n)	(GICD_BASE + 0x180 + 4 * (n))
#define GICD_ICPENDR(n)		(GICD_BASE + 0x280 + 4 * (n))
#define GICD_IPRIORITYR(n)	(GICD_BASE + 0x400 + 4 * (n))
#define GICD_ITARGETSR(n)	(GICD_BASE + 0x800 + 4 * (n))
#define GICD_ICFGR(n)		(GICD_BASE + 0xc00 + 4 * (n))
#define GICC_CTLR		(GICC_BASE + 0x000)
#define GICC_PMR		(GICC_BASE + 0x004)
#define GICC_BPR		(GICC_BASE + 0x008)
#define GICC_IAR		(GICC_BASE + 0x00c)
#define GICC_EOIR		(GICC_BASE + 0x010)

#define MAX_IRQ 288
static void (*handlers[MAX_IRQ])(void);
static unsigned nirq;
static uint32_t spurious;
const uint32_t *irq_frame;

void gic_init(void)
{
	writel(0, GICD_CTLR);
	nirq = ((readl(GICD_TYPER) & 0x1f) + 1) * 32;
	if (nirq > MAX_IRQ)
		nirq = MAX_IRQ;
	for (unsigned i = 0; i < nirq / 32; i++) {
		writel(~0u, GICD_ICENABLER(i));
		writel(~0u, GICD_ICPENDR(i));
	}
	for (unsigned i = 2; i < nirq / 16; i++)
		writel(0, GICD_ICFGR(i));
	for (unsigned i = 0; i < nirq / 4; i++) {
		writel(0xa0a0a0a0u, GICD_IPRIORITYR(i));
		if (i >= 8)
			writel(0x01010101u, GICD_ITARGETSR(i));
	}
	writel(1, GICD_CTLR);
	writel(0xff, GICC_PMR);
	writel(0, GICC_BPR);
	writel(1, GICC_CTLR);
}

/* The CPU interface and the banked private interrupt enables are lost
 * when the core is powered down. */
uint32_t gic_cpu_save(void)
{
	return readl(GICD_ISENABLER(0));
}

void gic_cpu_restore(uint32_t ppis)
{
	writel(ppis, GICD_ISENABLER(0));
	writel(0xff, GICC_PMR);
	writel(0, GICC_BPR);
	writel(1, GICC_CTLR);
}

void gic_enable(unsigned id, void (*fn)(void))
{
	if (id >= nirq)
		return;
	handlers[id] = fn;
	writel(BIT(id % 32), GICD_ISENABLER(id / 32));
}

void gic_set_edge(unsigned id)
{
	if (id < nirq)
		writel(readl(GICD_ICFGR(id / 16)) | BIT(id % 16 * 2 + 1), GICD_ICFGR(id / 16));
}

void gic_mask(unsigned id)
{
	if (id < nirq)
		writel(BIT(id % 32), GICD_ICENABLER(id / 32));
}

void gic_unmask(unsigned id)
{
	if (id < nirq)
		writel(BIT(id % 32), GICD_ISENABLER(id / 32));
}

void gic_disable(unsigned id)
{
	if (id >= nirq)
		return;
	writel(BIT(id % 32), GICD_ICENABLER(id / 32));
	handlers[id] = 0;
}

/* frame: {r0-r3, r12, pc} of the interrupted code, for handlers that
 * report where the main loop was. */
void irq_handler(const uint32_t *frame)
{
	irq_frame = frame;
	uint32_t iar = readl(GICC_IAR);
	unsigned id = iar & 0x3ff;
	if (id >= 1020) {
		spurious++;
		return;
	}
	if (id < nirq && handlers[id])
		handlers[id]();
	writel(iar, GICC_EOIR);
}
