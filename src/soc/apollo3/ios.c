/* The IO slave, an SPI slave on pads 0-3 set up as the vendor's firmware
 * has it for the application processor: 256 bytes of LRAM, the host's
 * direct area at 0x00-0x77 and a 128-byte FIFO from 0x80 that the host
 * reads through address 0x7f. A host write of the direct area and a
 * host read that reached the FIFO each end with an interrupt, handed on
 * with the status bits that ended it. */
#include "asteroid.h"

#define LRAM		IOS_BASE
#define FIFOPTR		(IOS_BASE + 0x100)
#define FIFOCFG		(IOS_BASE + 0x104)
#define FIFOTHR		(IOS_BASE + 0x108)
#define FUPD		(IOS_BASE + 0x10c)
#define FIFOCTR		(IOS_BASE + 0x110)
#define CFG		(IOS_BASE + 0x118)
#define IOINTCTL	(IOS_BASE + 0x120)
#define INTEN		(IOS_BASE + 0x200)
#define INTSTAT		(IOS_BASE + 0x204)
#define INTCLR		(IOS_BASE + 0x208)

#define CFG_SPI		BIT(0)
#define CFG_IFCEN	BIT(31)
#define FIFO_BASE	0x80u
#define RO_BASE		0x78u
#define FIFO_END	0x100u
#define FIFOCFG_VALUE	((RO_BASE / 8) << 24 | (FIFO_END / 8) << 8 | FIFO_BASE / 8)
#define FIFO_THRESHOLD	50u
#define FUPD_UPDATE	BIT(0)
#define IOINTCLR_ALL	BIT(16)

static void (*handler)(uint32_t status);
static uint32_t interrupts;

static void fifo_empty(void)
{
	writel(0, FIFOCTR);
	writel(FUPD_UPDATE, FUPD);
	writel(FIFO_BASE, FIFOPTR);
	writel(0, FUPD);
}

static void ios_irq(void)
{
	uint32_t st = readl(INTSTAT) & readl(INTEN);
	writel(st, INTCLR);
	if (st & IOS_WRITTEN)
		writel(IOINTCLR_ALL, IOINTCTL);
	interrupts++;
	if (handler)
		handler(st);
}

void ios_init(void (*fn)(uint32_t status))
{
	handler = fn;
	periph_power(PWR_IOS, true);
	pad_config(0, 1, PAD_INPUT);
	pad_config(1, 1, PAD_INPUT);
	pad_config(2, 1, PAD_STRONG);
	pad_config(3, 1, PAD_INPUT);
	writel(CFG_SPI, CFG);
	writel(FIFOCFG_VALUE, FIFOCFG);
	fifo_empty();
	writel(CFG_SPI | CFG_IFCEN, CFG);
	fifo_empty();
	writel(FIFO_THRESHOLD, FIFOTHR);
	writel(~0u, INTCLR);
	writel(readl(INTEN) | IOS_WRITTEN | IOS_READ, INTEN);
	irq_attach(IRQ_IOS, ios_irq);
}

void ios_read(unsigned off, uint8_t *p, unsigned n)
{
	const volatile uint8_t *lram = (const volatile uint8_t *)(uintptr_t)LRAM;
	for (unsigned i = 0; i < n && off + i < RO_BASE; i++)
		p[i] = lram[off + i];
}

/* Host reads through 0x7f take LRAM from FIFOPTR on whatever FIFOCTR
 * says; FIFOCTR only feeds the host's count registers. */
void ios_stage(const uint8_t *direct, unsigned dn, const uint8_t *fifo, unsigned fn)
{
	volatile uint8_t *lram = (volatile uint8_t *)(uintptr_t)LRAM;
	for (unsigned i = 0; i < dn && i < RO_BASE; i++)
		lram[i] = direct[i];
	fifo_empty();
	for (unsigned i = 0; i < fn && FIFO_BASE + i < FIFO_END; i++)
		lram[FIFO_BASE + i] = fifo[i];
	writel(fn, FIFOCTR);
}

uint32_t ios_interrupts(void) { return interrupts; }
