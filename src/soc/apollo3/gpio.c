/* The pads and their GPIOs. Each pad has a byte in PADREG (pull, input,
 * drive, function), a nibble in CFG (output mode, interrupt edge) and a
 * byte in ALTPADCFG (the high drive bit); PADREG and CFG take writes only
 * while unlocked by the pad key. All pads share one interrupt line. */
#include "asteroid.h"

#define PADREG(p)	(GPIO_BASE + 0x000 + ((p) & ~3u))
#define CFG(p)		(GPIO_BASE + 0x040 + (((p) >> 1) & ~3u))
#define PADKEY		(GPIO_BASE + 0x060)
#define RD(p)		(GPIO_BASE + 0x080 + 4 * ((p) / 32))
#define WTS(p)		(GPIO_BASE + 0x090 + 4 * ((p) / 32))
#define WTC(p)		(GPIO_BASE + 0x098 + 4 * ((p) / 32))
#define ENS(p)		(GPIO_BASE + 0x0a8 + 4 * ((p) / 32))
#define ENC(p)		(GPIO_BASE + 0x0b4 + 4 * ((p) / 32))
#define ALTPADCFG(p)	(GPIO_BASE + 0x0e0 + ((p) & ~3u))
#define INTEN(p)	(GPIO_BASE + 0x200 + 0x10 * ((p) / 32))
#define INTSTAT(p)	(GPIO_BASE + 0x204 + 0x10 * ((p) / 32))
#define INTCLR(p)	(GPIO_BASE + 0x208 + 0x10 * ((p) / 32))
#define KEY		0x73u
#define PAD_PULL	BIT(0)
#define PAD_INPEN	BIT(1)
#define PAD_STRNG	BIT(2)
#define PAD_FNC(f)	((f) << 3)
#define PAD_RSEL_1K5	(0u << 6)
#define OUT_DIS		0u
#define OUT_PUSHPULL	1u
#define OUT_OD		2u
/* INTD 0 latches a rising edge and 1 a falling one; INTEN decides
 * whether it interrupts. INCFG, left 0, would make the pad read 0. */
#define CFG_OUT(m)	((m) << 1)
#define CFG_INTD	BIT(3)

static void (*handlers[GPIO_COUNT])(unsigned n);

static void put(uint32_t reg, unsigned shift, uint32_t mask, uint32_t v)
{
	writel((readl(reg) & ~(mask << shift)) | (v & mask) << shift, reg);
}

static void set_pad(unsigned p, uint32_t padreg, uint32_t cfg, uint32_t alt)
{
	uint32_t flags = irq_save();
	writel(KEY, PADKEY);
	put(PADREG(p), (p & 3) * 8, 0xff, padreg);
	put(CFG(p), (p & 7) * 4, 0xf, cfg);
	put(ALTPADCFG(p), (p & 3) * 8, 0xff, alt);
	writel(0, PADKEY);
	irq_restore(flags);
}

void pad_config(unsigned p, unsigned func, unsigned flags)
{
	if (p >= GPIO_COUNT)
		return;
	uint32_t padreg = PAD_FNC(func & 7) | (flags & PAD_INPUT ? PAD_INPEN : 0) | (flags & PAD_STRONG ? PAD_STRNG : 0) |
			  (flags & (PAD_PULL_UP | PAD_I2C_PULL) ? PAD_PULL : 0) | PAD_RSEL_1K5;
	uint32_t cfg = flags & PAD_OPEN_DRAIN ? CFG_OUT(OUT_OD) : 0;
	if (flags & BIT(5))
		cfg = CFG_OUT(flags >> 6 & 3);
	set_pad(p, padreg, cfg, flags & PAD_STRONG ? 1 : 0);
}

void pad_set(const struct board_mcu_pad *p)
{
	if (p->flags & (PAD_OUT_HIGH | PAD_OUT_LOW)) {
		gpio_set(p->pad, (p->flags & PAD_OUT_HIGH) != 0);
		gpio_config(p->pad, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	} else
		pad_config(p->pad, p->func, p->flags);
}

/* A GPIO on any pad; func is ignored, as every pad has the GPIO on 3.
 * Interrupts stay off until gpio_irq_config sets an edge. */
void gpio_config(unsigned n, unsigned func, unsigned dir, unsigned pull, unsigned drive_ma)
{
	if (n >= GPIO_COUNT)
		return;
	uint32_t padreg = PAD_FNC(PAD_FUNC_GPIO) | PAD_INPEN | (pull == GPIO_PULL_UP ? PAD_PULL : 0) |
			  (drive_ma >= 4 && drive_ma < 8 ? PAD_STRNG : drive_ma >= 12 ? PAD_STRNG : 0);
	uint32_t alt = drive_ma >= 8 ? 1 : 0;
	uint32_t cfg = dir == GPIO_OUTPUT ? CFG_OUT(OUT_PUSHPULL) : CFG_OUT(OUT_DIS);
	set_pad(n, padreg, cfg, alt);
}

unsigned gpio_get(unsigned n)
{
	return n < GPIO_COUNT ? readl(RD(n)) >> (n % 32) & 1 : 0;
}

void gpio_set(unsigned n, unsigned v)
{
	if (n < GPIO_COUNT)
		writel(BIT(n % 32), v ? WTS(n) : WTC(n));
}

uint32_t gpio_raw_config(unsigned n)
{
	if (n >= GPIO_COUNT)
		return 0;
	return (readl(PADREG(n)) >> ((n & 3) * 8) & 0xff) | (readl(CFG(n)) >> ((n & 7) * 4) & 0xf) << 8 |
	       (readl(ALTPADCFG(n)) >> ((n & 3) * 8) & 0xff) << 16;
}

static void gpio_irq(void)
{
	for (unsigned bank = 0; bank < 2; bank++) {
		unsigned p0 = bank * 32;
		uint32_t st = readl(INTSTAT(p0)) & readl(INTEN(p0));
		writel(st, INTCLR(p0));
		while (st) {
			unsigned b = (unsigned)__builtin_ctz(st);
			st &= st - 1;
			irq_last = IRQ_LAST_GPIO + p0 + b;
			if (p0 + b < GPIO_COUNT && handlers[p0 + b])
				handlers[p0 + b](p0 + b);
		}
	}
}

void gpio_init(void)
{
	writel(0, INTEN(0));
	writel(0, INTEN(32));
	writel(~0u, INTCLR(0));
	writel(~0u, INTCLR(32));
}

void gpio_irq_init(void) { irq_attach(IRQ_GPIO, gpio_irq); }

/* The pads latch edges only; a level asks for the edge into it. */
void gpio_irq_config(unsigned n, unsigned type, void (*fn)(unsigned n))
{
	if (n >= GPIO_COUNT)
		return;
	handlers[n] = fn;
	bool falling = type == GPIO_IRQ_EDGE_FALLING || type == GPIO_IRQ_LEVEL_LOW;
	uint32_t flags = irq_save();
	writel(KEY, PADKEY);
	uint32_t cur = readl(CFG(n)) >> ((n & 7) * 4) & 0xf;
	put(CFG(n), (n & 7) * 4, 0xf, (cur & CFG_OUT(3)) | (falling ? CFG_INTD : 0));
	writel(0, PADKEY);
	irq_restore(flags);
	writel(BIT(n % 32), INTCLR(n));
}

void gpio_irq_enable(unsigned n, bool on)
{
	if (n >= GPIO_COUNT)
		return;
	uint32_t flags = irq_save();
	writel(on ? readl(INTEN(n)) | BIT(n % 32) : readl(INTEN(n)) & ~BIT(n % 32), INTEN(n));
	irq_restore(flags);
}

void gpio_irq_ack(unsigned n)
{
	if (n < GPIO_COUNT)
		writel(BIT(n % 32), INTCLR(n));
}
