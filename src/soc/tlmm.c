/* Top-level mode mux: one configuration word per pin (pull, function,
 * drive strength, output enable) and an in/out word. */
#include "asteroid.h"

void tlmm_config(unsigned n, unsigned func, unsigned dir, unsigned pull, unsigned drive_ma)
{
	uint32_t v = (pull & 3) | (func & 0xf) << 2 | ((drive_ma / 2 - 1) & 7) << 6 | (dir ? BIT(9) : 0);
	writel(v, TLMM_GPIO_CFG(n));
}

unsigned tlmm_get(unsigned n)
{
	return readl(TLMM_GPIO_IO(n)) & 1;
}

void tlmm_set(unsigned n, unsigned v)
{
	writel(v ? BIT(1) : 0, TLMM_GPIO_IO(n));
}

/* Pin interrupts: each pin has an interrupt configuration word (enable,
 * polarity, detection, raw status, target processor) and a status word;
 * all pins share one summary interrupt, SPI 208. */
#define TLMM_INTR_CFG(n)	(TLMM_BASE + 0x1008 + (n) * 0x10)
#define TLMM_INTR_STATUS(n)	(TLMM_BASE + 0x100c + (n) * 0x10)
#define INTR_ENABLE		BIT(0)
#define INTR_POLARITY_HIGH	BIT(1)
#define INTR_DETECT_SHIFT	2
#define INTR_RAW_STATUS		BIT(4)
#define INTR_TARGET_APPS	(4u << 5)
#define IRQ_TLMM_SUMMARY	(32 + 208)
#define MAX_PIN_IRQS		4

static struct { unsigned pin; void (*fn)(unsigned n); } pin_irqs[MAX_PIN_IRQS];
static unsigned npin_irqs;

static void summary_irq(void)
{
	for (unsigned i = 0; i < npin_irqs; i++) {
		unsigned n = pin_irqs[i].pin;
		if ((readl(TLMM_INTR_CFG(n)) & INTR_ENABLE) && (readl(TLMM_INTR_STATUS(n)) & 1))
			pin_irqs[i].fn(n);
	}
}

void tlmm_irq_init(void)
{
	gic_enable(IRQ_TLMM_SUMMARY, summary_irq);
}

void tlmm_irq_config(unsigned n, unsigned type, void (*fn)(unsigned n))
{
	uint32_t v = INTR_TARGET_APPS | INTR_RAW_STATUS;
	switch (type) {
	case TLMM_IRQ_LEVEL_HIGH: v |= INTR_POLARITY_HIGH; break;
	case TLMM_IRQ_EDGE_RISING: v |= INTR_POLARITY_HIGH | 1u << INTR_DETECT_SHIFT; break;
	case TLMM_IRQ_EDGE_FALLING: v |= INTR_POLARITY_HIGH | 2u << INTR_DETECT_SHIFT; break;
	default: break;
	}
	unsigned i = 0;
	while (i < npin_irqs && pin_irqs[i].pin != n)
		i++;
	if (i < MAX_PIN_IRQS) {
		pin_irqs[i].pin = n;
		pin_irqs[i].fn = fn;
		if (i == npin_irqs)
			npin_irqs++;
	}
	writel(v, TLMM_INTR_CFG(n));
	writel(0, TLMM_INTR_STATUS(n));
}

void tlmm_irq_enable(unsigned n, bool on)
{
	uint32_t v = readl(TLMM_INTR_CFG(n));
	writel(on ? v | INTR_ENABLE : v & ~INTR_ENABLE, TLMM_INTR_CFG(n));
}

void tlmm_irq_ack(unsigned n)
{
	writel(0, TLMM_INTR_STATUS(n));
}
