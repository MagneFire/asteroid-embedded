/* GPIO: six banks of 32 pins, each with its own interrupt. Which pin of
 * the chip a GPIO is on, its pull and whether it can wake the chip are
 * the pad's settings, not the bank's: the bootloader has muxed the pads
 * for this board and pads_init changes the ones the board table lists,
 * so gpio_config only sets the direction. */
#include "asteroid.h"

#define GPIO_SYSCONFIG		0x10
#define GPIO_IRQSTATUS1		0x18
#define GPIO_IRQENABLE1		0x1c
#define GPIO_OE			0x34
#define GPIO_DATAIN		0x38
#define GPIO_DATAOUT		0x3c
#define GPIO_LEVELDETECT0	0x40
#define GPIO_LEVELDETECT1	0x44
#define GPIO_RISINGDETECT	0x48
#define GPIO_FALLINGDETECT	0x4c
#define GPIO_CLEARIRQENABLE1	0x60
#define GPIO_SETIRQENABLE1	0x64
#define GPIO_CLEARWKUENA	0x80
#define GPIO_SETWKUENA		0x84
#define GPIO_CLEARDATAOUT	0x90
#define GPIO_SETDATAOUT		0x94

static const uint32_t banks[] = { 0x48310000u, 0x49050000u, 0x49052000u, 0x49054000u, 0x49056000u, 0x49058000u };
#define BANK(n)	(banks[(n) / 32])
#define PIN(n)	BIT((n) % 32)

void pad_config(uint32_t off, uint32_t value)
{
	*(volatile uint16_t *)(uintptr_t)CONTROL_PADCONF(off) = (uint16_t)value;
}

uint32_t pad_read(uint32_t off)
{
	return *(volatile uint16_t *)(uintptr_t)CONTROL_PADCONF(off);
}

void gpio_config(unsigned n, unsigned func, unsigned dir, unsigned pull, unsigned drive_ma)
{
	if (n >= GPIO_COUNT)
		return;
	uint32_t flags = irq_save();
	uint32_t oe = readl(BANK(n) + GPIO_OE);
	writel(dir == GPIO_OUTPUT ? oe & ~PIN(n) : oe | PIN(n), BANK(n) + GPIO_OE);
	irq_restore(flags);
}

unsigned gpio_get(unsigned n)
{
	if (n >= GPIO_COUNT)
		return 0;
	uint32_t out = !(readl(BANK(n) + GPIO_OE) & PIN(n));
	return !!(readl(BANK(n) + (out ? GPIO_DATAOUT : GPIO_DATAIN)) & PIN(n));
}

void gpio_set(unsigned n, unsigned v)
{
	if (n < GPIO_COUNT)
		writel(PIN(n), BANK(n) + (v ? GPIO_SETDATAOUT : GPIO_CLEARDATAOUT));
}

/* Output enable (bit 0, set for an output), the output latch (1) and
 * the level at the pin (2). */
uint32_t gpio_raw_config(unsigned n)
{
	if (n >= GPIO_COUNT)
		return 0;
	uint32_t out = !(readl(BANK(n) + GPIO_OE) & PIN(n)), latch = !!(readl(BANK(n) + GPIO_DATAOUT) & PIN(n));
	uint32_t level = !!(readl(BANK(n) + GPIO_DATAIN) & PIN(n));
	return out | latch << 1 | level << 2;
}

#define MAX_PIN_IRQS	8

static struct { unsigned pin; bool edge; void (*fn)(unsigned n); } pin_irqs[MAX_PIN_IRQS];
static unsigned npin_irqs;

/* An edge is acknowledged before its handler, so that one arriving
 * while it runs is kept; a level after it, once the handler has
 * silenced or masked the source. */
static void bank_irq(unsigned bank)
{
	uint32_t base = banks[bank], pending = readl(base + GPIO_IRQSTATUS1) & readl(base + GPIO_IRQENABLE1);
	for (unsigned i = 0; i < npin_irqs; i++) {
		unsigned n = pin_irqs[i].pin;
		if (n / 32 != bank || !(pending & PIN(n)))
			continue;
		if (pin_irqs[i].edge)
			writel(PIN(n), base + GPIO_IRQSTATUS1);
		pin_irqs[i].fn(n);
		if (!pin_irqs[i].edge)
			writel(PIN(n), base + GPIO_IRQSTATUS1);
		pending &= ~PIN(n);
	}
	if (pending) {
		writel(pending, base + GPIO_CLEARIRQENABLE1);
		writel(pending, base + GPIO_IRQSTATUS1);
	}
}

static void bank1_irq(void) { bank_irq(0); }
static void bank2_irq(void) { bank_irq(1); }
static void bank3_irq(void) { bank_irq(2); }
static void bank4_irq(void) { bank_irq(3); }
static void bank5_irq(void) { bank_irq(4); }
static void bank6_irq(void) { bank_irq(5); }

void gpio_init(void)
{
	clock_enable(CM_WKUP, WKUP_GPIO1, true);
	clock_enable(CM_PER, PER_GPIO(2) | PER_GPIO(3) | PER_GPIO(4) | PER_GPIO(5) | PER_GPIO(6), true);
	for (unsigned b = 0; b < ARRAY_SIZE(banks); b++) {
		writel(0, banks[b] + GPIO_IRQENABLE1);
		writel(~0u, banks[b] + GPIO_CLEARWKUENA);
		writel(0, banks[b] + GPIO_LEVELDETECT0);
		writel(0, banks[b] + GPIO_LEVELDETECT1);
		writel(0, banks[b] + GPIO_RISINGDETECT);
		writel(0, banks[b] + GPIO_FALLINGDETECT);
		writel(~0u, banks[b] + GPIO_IRQSTATUS1);
		/* Smart idle with wake-up, as the vendor's driver has it. */
		writel(0x2 << 3 | BIT(2), banks[b] + GPIO_SYSCONFIG);
	}
}

void gpio_irq_init(void)
{
	static void (*const fns[])(void) = { bank1_irq, bank2_irq, bank3_irq, bank4_irq, bank5_irq, bank6_irq };
	for (unsigned b = 0; b < ARRAY_SIZE(banks); b++)
		irq_attach(IRQ_GPIO(b + 1), fns[b]);
}

void gpio_irq_config(unsigned n, unsigned type, void (*fn)(unsigned n))
{
	static const uint16_t detect[] = {
		[GPIO_IRQ_LEVEL_LOW] = GPIO_LEVELDETECT0, [GPIO_IRQ_LEVEL_HIGH] = GPIO_LEVELDETECT1,
		[GPIO_IRQ_EDGE_FALLING] = GPIO_FALLINGDETECT, [GPIO_IRQ_EDGE_RISING] = GPIO_RISINGDETECT,
	};
	if (n >= GPIO_COUNT || type >= ARRAY_SIZE(detect))
		return;
	uint32_t flags = irq_save();
	unsigned i = 0;
	while (i < npin_irqs && pin_irqs[i].pin != n)
		i++;
	if (i < MAX_PIN_IRQS) {
		pin_irqs[i].pin = n;
		pin_irqs[i].fn = fn;
		pin_irqs[i].edge = type == GPIO_IRQ_EDGE_FALLING || type == GPIO_IRQ_EDGE_RISING;
		if (i == npin_irqs)
			npin_irqs++;
	}
	writel(PIN(n), BANK(n) + GPIO_CLEARIRQENABLE1);
	for (unsigned t = 0; t < ARRAY_SIZE(detect); t++) {
		uint32_t v = readl(BANK(n) + detect[t]);
		writel(t == type ? v | PIN(n) : v & ~PIN(n), BANK(n) + detect[t]);
	}
	/* An edge on a bank whose clock has idled is only seen with this. */
	writel(PIN(n), BANK(n) + GPIO_SETWKUENA);
	writel(PIN(n), BANK(n) + GPIO_IRQSTATUS1);
	irq_restore(flags);
}

void gpio_irq_enable(unsigned n, bool on)
{
	if (n < GPIO_COUNT)
		writel(PIN(n), BANK(n) + (on ? GPIO_SETIRQENABLE1 : GPIO_CLEARIRQENABLE1));
}

void gpio_irq_ack(unsigned n)
{
	if (n < GPIO_COUNT)
		writel(PIN(n), BANK(n) + GPIO_IRQSTATUS1);
}
