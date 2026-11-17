/* The six IO masters, each an I2C or SPI master with a 32-byte FIFO each
 * way and DMA. A command names the transfer: direction, length, up to 3
 * bytes sent ahead of it (the register of an I2C read, which gives the
 * repeated start), the chip select and whether to hold the bus after.
 * Small transfers go through the FIFO by hand; iom_spi_write_start lets DMA
 * feed a long write. */
#include "asteroid.h"

#define IOM(n, off)	(IOM_BASE(n) + (off))
#define FIFOPTR		0x100
#define FIFOTHR		0x104
#define FIFOPOP		0x108
#define FIFOPUSH	0x10c
#define FIFOCTRL	0x110
#define INTEN		0x200
#define INTSTAT		0x204
#define INTCLR		0x208
#define CLKCFG		0x210
#define SUBMODCTRL	0x214
#define CMD		0x218
#define DCX		0x21c
#define OFFSETHI	0x220
#define DMATRIGEN	0x240
#define DMACFG		0x280
#define DMATOTCOUNT	0x288
#define DMATARGADDR	0x28c
#define DMASTAT		0x290
#define STATUS		0x2b4
#define MSPICFG		0x300
#define MI2CCFG		0x400
#define DEVCFG		0x404

#define CMD_WRITE	1u
#define CMD_READ	2u
#define CMD_CONT	BIT(7)
#define CMD_OFFSETCNT(n) ((n) << 5)
#define CMD_TSIZE(n)	((n) << 8)
#define CMD_CS(n)	((n) << 20)
#define CMD_OFFSETLO(b)	((uint32_t)(b) << 24)
#define STATUS_IDLE	BIT(2)
#define STATUS_CMDACT	BIT(1)
#define INT_CMDCMP	BIT(0)
#define INT_NAK		BIT(4)
#define INT_ERRORS	(BIT(2) | BIT(3) | BIT(4) | BIT(5) | BIT(6) | BIT(9) | BIT(11))
#define FIFORSTN	BIT(1)
#define MSPI_SPOL	BIT(0)
#define MSPI_SPHA	BIT(1)
#define CLK_IOCLKEN	BIT(0)
#define CLK_FSEL(n)	((n) << 8)
#define CLK_DIV3	BIT(11)
#define CLK_DIVEN	BIT(12)
#define CLK_LOWPER(n)	((n) << 16)
#define CLK_TOTPER(n)	((uint32_t)(n) << 24)
#define SMOD0EN		BIT(0)	/* the SPI master */
#define SMOD1EN		BIT(4)	/* the I2C master */
#define DMA_EN		BIT(0)
#define DMA_M2P		BIT(1)
#define DMA_HIGH	BIT(8)
#define DMASTAT_ERR	BIT(2)
#define DTHREN		BIT(1)
#define MAX_TSIZE	4095u
#define BUSES		6

struct bus {
	bool on, spi, dma;
	uint32_t timeout_us, errors, naks;
	/* An I2C transfer started with i2c_start, done on the next poll. */
	i2c_done_fn done;
	void *ctx;
	int result;
};
static struct bus buses[BUSES];

void periph_power(uint32_t bits, bool on)
{
	uint32_t flags = irq_save();
	uint32_t v = readl(PWRCTRL_DEVPWREN);
	writel(on ? v | bits : v & ~bits, PWRCTRL_DEVPWREN);
	irq_restore(flags);
	if (on)
		delay_us(20);
}

static bool idle(unsigned n)
{
	return (readl(IOM(n, STATUS)) & (STATUS_IDLE | STATUS_CMDACT)) == STATUS_IDLE;
}

static int wait_idle(unsigned n, uint32_t us)
{
	uint32_t t0 = time_us();
	while (!idle(n))
		if (time_us() - t0 > us)
			return -1;
	return 0;
}

static void power_up(unsigned n)
{
	periph_power(PWR_IOM(n), true);
	writel(0, IOM(n, FIFOCTRL));
	delay_us(2);
	writel(FIFORSTN, IOM(n, FIFOCTRL));
	writel(16u << 8 | 16u, IOM(n, FIFOTHR));
	writel(0, IOM(n, INTEN));
	writel(~0u, IOM(n, INTCLR));
}

/* After a NAK or a bus error the master stays stuck until its FIFO and
 * I2C side are reset. */
static void recover(unsigned n)
{
	uint32_t sub = readl(IOM(n, SUBMODCTRL));
	writel(0, IOM(n, DMACFG));
	writel(0, IOM(n, SUBMODCTRL));
	writel(0, IOM(n, FIFOCTRL));
	delay_us(10);
	writel(FIFORSTN, IOM(n, FIFOCTRL));
	writel(sub, IOM(n, SUBMODCTRL));
	writel(~0u, IOM(n, INTCLR));
}

/* One command through the FIFO. off holds up to 3 bytes sent first,
 * the first of them highest. */
static int xfer(unsigned n, unsigned cs, uint32_t dev, uint32_t off, unsigned offn, bool rx, uint8_t *p, unsigned len,
		bool cont)
{
	struct bus *b = &buses[n];
	if (len > MAX_TSIZE)
		return -1;
	if (wait_idle(n, b->timeout_us))
		recover(n);
	writel(0, IOM(n, DMACFG));
	writel(~0u, IOM(n, INTCLR));
	writel(dev, IOM(n, DEVCFG));
	writel(0, IOM(n, DCX));
	writel(off >> 8 & 0xffff, IOM(n, OFFSETHI));
	uint32_t cmd = (rx ? CMD_READ : CMD_WRITE) | CMD_TSIZE(len) | CMD_CS(cs) | CMD_OFFSETCNT(offn) |
		       CMD_OFFSETLO(off & 0xff) | (cont ? CMD_CONT : 0);
	unsigned left = len;
	uint32_t t0 = time_us();
	if (!rx) {
		/* Prime the FIFO, so that the command never starts on an
		 * empty one. */
		while (left && (readl(IOM(n, FIFOPTR)) >> 8 & 0xff) >= 4) {
			uint32_t w = 0;
			for (unsigned i = 0; i < 4 && i < left; i++)
				w |= (uint32_t)p[i] << (8 * i);
			writel(w, IOM(n, FIFOPUSH));
			p += left < 4 ? left : 4;
			left -= left < 4 ? left : 4;
		}
	}
	writel(cmd, IOM(n, CMD));
	while (left) {
		uint32_t ptr = readl(IOM(n, FIFOPTR)), st = readl(IOM(n, INTSTAT));
		if (st & INT_ERRORS)
			break;
		if (rx && ((ptr >> 16 & 0xff) >= 4 || ((ptr >> 16 & 0xff) >= left && (st & INT_CMDCMP)))) {
			uint32_t w = readl(IOM(n, FIFOPOP));
			for (unsigned i = 0; i < 4 && left; i++, left--)
				*p++ = (uint8_t)(w >> (8 * i));
			t0 = time_us();
		} else if (!rx && (ptr >> 8 & 0xff) >= 4) {
			uint32_t w = 0;
			for (unsigned i = 0; i < 4 && i < left; i++)
				w |= (uint32_t)p[i] << (8 * i);
			writel(w, IOM(n, FIFOPUSH));
			p += left < 4 ? left : 4;
			left -= left < 4 ? left : 4;
			t0 = time_us();
		} else if ((st & INT_CMDCMP) || time_us() - t0 > b->timeout_us)
			break;
	}
	int r = wait_idle(n, b->timeout_us);
	uint32_t st = readl(IOM(n, INTSTAT));
	if (st & INT_NAK)
		b->naks++;
	if (r || left || (st & INT_ERRORS)) {
		b->errors++;
		recover(n);
		return st & INT_NAK ? -6 : -5;
	}
	writel(~0u, IOM(n, INTCLR));
	return 0;
}

int i2c_init(unsigned n, unsigned sda, unsigned scl, unsigned func, uint32_t bus_hz)
{
	if (n >= BUSES)
		return -1;
	struct bus *b = &buses[n];
	/* A device left halfway through a read holds SDA low: clock it out. */
	gpio_config(sda, 0, GPIO_INPUT, GPIO_PULL_UP, 2);
	gpio_set(scl, 1);
	gpio_config(scl, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	for (int i = 0; i < 9 && !gpio_get(sda); i++) {
		gpio_set(scl, 0);
		delay_us(5);
		gpio_set(scl, 1);
		delay_us(5);
	}
	pad_config(sda, func, PAD_INPUT | PAD_I2C_PULL | PAD_OPEN_DRAIN);
	pad_config(scl, func, PAD_INPUT | PAD_I2C_PULL | PAD_OPEN_DRAIN);
	power_up(n);
	/* 400 kHz: 48 MHz / 2 over 30 periods, and the HAL's timing. */
	writel(CLK_TOTPER(0x1d) | CLK_LOWPER(0x0e) | CLK_DIVEN | CLK_FSEL(2) | CLK_IOCLKEN, IOM(n, CLKCFG));
	writel(3u << 16 | 15u << 12 | 2u << 8 | BIT(6) | 3u << 4, IOM(n, MI2CCFG));
	writel(SMOD1EN, IOM(n, SUBMODCTRL));
	b->on = true;
	b->spi = false;
	b->timeout_us = 20000;
	(void)bus_hz;
	return 0;
}

void i2c_set_timeout(unsigned n, uint32_t stretch_ms)
{
	if (n < BUSES)
		buses[n].timeout_us = 20000 + stretch_ms * 1000;
}

int i2c_xfer(unsigned n, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	if (n >= BUSES || !buses[n].on || buses[n].spi)
		return -1;
	if (!rn)
		return xfer(n, 0, addr, 0, 0, false, (uint8_t *)(uintptr_t)w, wn, false);
	if (wn <= 3) {
		uint32_t off = 0;
		for (unsigned i = 0; i < wn; i++)
			off = off << 8 | w[i];
		return xfer(n, 0, addr, off, wn, true, r, rn, false);
	}
	int e = xfer(n, 0, addr, 0, 0, false, (uint8_t *)(uintptr_t)w, wn, true);
	return e ? e : xfer(n, 0, addr, 0, 0, true, r, rn, false);
}

int i2c_start(unsigned n, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn, i2c_done_fn done, void *ctx)
{
	if (n >= BUSES || buses[n].done)
		return -1;
	buses[n].result = i2c_xfer(n, addr, w, wn, r, rn);
	buses[n].ctx = ctx;
	buses[n].done = done;
	return 0;
}

bool i2c_busy(unsigned n) { return n < BUSES && buses[n].done; }

void i2c_poll(unsigned n)
{
	if (n >= BUSES || !buses[n].done)
		return;
	i2c_done_fn fn = buses[n].done;
	buses[n].done = NULL;
	fn(buses[n].result, buses[n].ctx);
}

/* The clock divider as the HAL works it out: 48 MHz over 2^(fsel - 1),
 * over 3 or not, then over the period count. */
static uint32_t spi_clock(uint32_t hz, bool longer_high)
{
	uint32_t div = (CORE_HZ + hz - 1) / hz;
	unsigned pow = (unsigned)__builtin_ctz(div);
	if (pow > 6)
		pow = 6;
	bool div3 = hz < CORE_HZ / 16384 || (hz >= CORE_HZ / 3 && hz <= CORE_HZ / 2 - 1);
	uint32_t denom = (1u << pow) * (div3 ? 3 : 1);
	uint32_t totper = (div + denom - 1) / denom;
	unsigned v1 = 31 - (unsigned)__builtin_clz(totper);
	unsigned fsel = (v1 > 7 ? v1 + pow - 7 : pow) + 1;
	if (fsel > 7)
		fsel = 7;
	if (v1 > 7) {
		uint32_t full = totper;
		totper >>= v1 - 7;
		if (full % (1u << (v1 - 7)))
			totper++;
	}
	bool diven = !(hz >= CORE_HZ / 4 || (1u << (fsel - 1)) == div);
	uint32_t lowper = totper < 2 ? 0 : longer_high ? (totper - 2) / 2 : (totper - 1) / 2;
	return CLK_FSEL(fsel) | (div3 ? CLK_DIV3 : 0) | (diven ? CLK_DIVEN : 0) | CLK_LOWPER(lowper) |
	       CLK_TOTPER(totper - 1) | CLK_IOCLKEN;
}

int iom_spi_init(unsigned n, uint32_t hz, unsigned mode)
{
	if (n >= BUSES || !hz)
		return -1;
	struct bus *b = &buses[n];
	power_up(n);
	writel(spi_clock(hz, mode & 2), IOM(n, CLKCFG));
	writel((mode & 2 ? MSPI_SPOL : 0) | (mode & 1 ? MSPI_SPHA : 0), IOM(n, MSPICFG));
	writel(SMOD0EN, IOM(n, SUBMODCTRL));
	writel(DTHREN, IOM(n, DMATRIGEN));
	b->on = true;
	b->spi = true;
	b->timeout_us = 20000;
	return 0;
}

int iom_spi_write(unsigned n, unsigned cs, const void *p, unsigned len, bool cont)
{
	if (n >= BUSES || !buses[n].spi || iom_spi_wait(n))
		return -1;
	return xfer(n, cs, 0, 0, 0, false, (uint8_t *)(uintptr_t)p, len, cont);
}

int iom_spi_read(unsigned n, unsigned cs, void *p, unsigned len, bool cont)
{
	if (n >= BUSES || !buses[n].spi || iom_spi_wait(n))
		return -1;
	return xfer(n, cs, 0, 0, 0, true, p, len, cont);
}

int iom_spi_write_start(unsigned n, unsigned cs, const void *p, unsigned len, bool cont)
{
	if (n >= BUSES || !buses[n].spi || !len || len > MAX_TSIZE || iom_spi_wait(n))
		return -1;
	writel(0, IOM(n, DMACFG));
	writel(~0u, IOM(n, INTCLR));
	writel(0, IOM(n, OFFSETHI));
	writel(len, IOM(n, DMATOTCOUNT));
	writel((uint32_t)(uintptr_t)p, IOM(n, DMATARGADDR));
	writel(DMA_HIGH | DMA_M2P | DMA_EN, IOM(n, DMACFG));
	writel(CMD_WRITE | CMD_TSIZE(len) | CMD_CS(cs) | (cont ? CMD_CONT : 0), IOM(n, CMD));
	buses[n].dma = true;
	return 0;
}

bool iom_spi_busy(unsigned n)
{
	return n < BUSES && buses[n].dma && !idle(n);
}

void iom_stop(unsigned n)
{
	if (n >= BUSES || !buses[n].on)
		return;
	iom_spi_wait(n);
	wait_idle(n, 100000);
	buses[n].on = false;
	periph_power(PWR_IOM(n), false);
}

int iom_spi_wait(unsigned n)
{
	if (n >= BUSES || !buses[n].dma)
		return 0;
	int r = wait_idle(n, 100000);
	buses[n].dma = false;
	uint32_t err = readl(IOM(n, DMASTAT)) & DMASTAT_ERR;
	writel(0, IOM(n, DMACFG));
	if (r || err) {
		buses[n].errors++;
		recover(n);
		return -5;
	}
	writel(~0u, IOM(n, INTCLR));
	return 0;
}
