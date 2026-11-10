/* I2C master on the OMAP's three controllers. A transfer is a write, a
 * read, or a write followed by a read after a repeated start, driven
 * from the interrupt a byte at a time: the transmit and receive
 * thresholds stay at one, which keeps the draining interrupts and
 * their errata out of it. The completion callback runs in interrupt
 * context. The stop is asked for once the start has gone out, as the
 * controller mishandles both in one write. A transfer that gets
 * nowhere is given up from the main loop, with a controller reset and
 * nine clocks for a slave left holding the data line; a bus may allow
 * longer for a slave that holds the clock while it prepares an
 * answer. */
#include <string.h>
#include "asteroid.h"

#define I2C_IE		0x04
#define I2C_STAT	0x08
#define I2C_SYSS	0x10
#define I2C_BUF		0x14
#define I2C_CNT		0x18
#define I2C_DATA	0x1c
#define I2C_SYSC	0x20
#define I2C_CON		0x24
#define I2C_SA		0x2c
#define I2C_PSC		0x30
#define I2C_SCLL	0x34
#define I2C_SCLH	0x38
#define I2C_SYSTEST	0x3c

#define ST_AL		BIT(0)
#define ST_NACK		BIT(1)
#define ST_ARDY		BIT(2)
#define ST_RRDY		BIT(3)
#define ST_XRDY		BIT(4)
#define ST_BB		BIT(12)
#define CON_EN		BIT(15)
#define CON_MST		BIT(10)
#define CON_TRX		BIT(9)
#define CON_STP		BIT(1)
#define CON_STT		BIT(0)
#define BUF_CLEAR	(BIT(14) | BIT(6))
#define TEST_EN		BIT(15)
#define TEST_IO		(3u << 12)
#define TEST_SCL	BIT(2)
#define TEST_SDA	BIT(0)

#define FCLK_KHZ	96000
#define TIMEOUT_MS	20
#define MAX_WRITE	64
#define MAX_READ	256
#define BUS_CLEAR_CLOCKS 9
#define BUS_CLEAR_HALF_US 5

struct bus {
	uint32_t base;
	uint16_t psc, scll, sclh;
	bool ready;
	uint32_t stretch_ms;		/* allowed while the slave holds the clock, 0 for none */
	uint32_t logged_t;
	struct {
		volatile bool busy;
		uint8_t w[MAX_WRITE];
		unsigned wn, wi;
		uint8_t *r;
		unsigned rn, ri;
		bool reading;
		uint8_t addr;
		uint32_t started;
		i2c_done_fn done;
		void *ctx;
	} xfer;
	/* A failure seen in interrupt context, logged by i2c_poll. */
	volatile uint16_t failed_stat;
};

static struct bus buses[3] = { { .base = 0x48070000u }, { .base = 0x48072000u }, { .base = 0x48060000u } };
static const uint8_t irqs[3] = { IRQ_I2C1, IRQ_I2C2, IRQ_I2C3 };

static uint16_t rd(const struct bus *b, uint32_t reg) { return *(volatile uint16_t *)(uintptr_t)(b->base + reg); }
static void wr(const struct bus *b, uint32_t reg, uint16_t v) { *(volatile uint16_t *)(uintptr_t)(b->base + reg) = v; }

static struct bus *bus(unsigned n)
{
	return n >= 1 && n <= 3 && buses[n - 1].ready ? &buses[n - 1] : NULL;
}

static void setup(const struct bus *b)
{
	wr(b, I2C_CON, 0);
	wr(b, I2C_SYSC, BIT(1));
	wr(b, I2C_CON, CON_EN);
	for (unsigned i = 0; i < 100000 && !(rd(b, I2C_SYSS) & 1); i++)
		;
	wr(b, I2C_CON, 0);
	/* No idle: its clocks are gated by hand around use instead. */
	wr(b, I2C_SYSC, 1u << 3);
	wr(b, I2C_PSC, b->psc);
	wr(b, I2C_SCLL, b->scll);
	wr(b, I2C_SCLH, b->sclh);
	wr(b, I2C_BUF, BUF_CLEAR);
	wr(b, I2C_CON, CON_EN);
	wr(b, I2C_IE, 0);
	wr(b, I2C_STAT, 0xffff);
}

/* The stop comes after the start has left. */
static void start(const struct bus *b, uint16_t con)
{
	wr(b, I2C_CON, con | CON_STT);
	if (!(con & CON_STP))
		return;
	for (unsigned i = 0; i < 20000 && (rd(b, I2C_CON) & CON_STT); i++)
		;
	wr(b, I2C_CON, con);
}

static void start_read(struct bus *b)
{
	b->xfer.reading = true;
	wr(b, I2C_CNT, (uint16_t)b->xfer.rn);
	start(b, CON_EN | CON_MST | CON_STP);
}

static void finish(struct bus *b, int err)
{
	i2c_done_fn done = b->xfer.done;
	void *ctx = b->xfer.ctx;
	wr(b, I2C_IE, 0);
	b->xfer.busy = false;
	if (done)
		done(err, ctx);
}

static void bus_irq(struct bus *b)
{
	uint16_t stat = rd(b, I2C_STAT) & rd(b, I2C_IE);
	if (!b->xfer.busy) {
		wr(b, I2C_IE, 0);
		wr(b, I2C_STAT, stat);
		return;
	}
	if (stat & (ST_NACK | ST_AL)) {
		if (stat & ST_NACK)
			wr(b, I2C_CON, rd(b, I2C_CON) | CON_STP);
		wr(b, I2C_STAT, stat);
		wr(b, I2C_BUF, BUF_CLEAR);
		b->failed_stat = stat;
		finish(b, -1);
		return;
	}
	if (stat & ST_RRDY) {
		uint8_t c = (uint8_t)rd(b, I2C_DATA);
		if (b->xfer.ri < b->xfer.rn)
			b->xfer.r[b->xfer.ri++] = c;
		wr(b, I2C_STAT, ST_RRDY);
	}
	if (stat & ST_XRDY) {
		if (b->xfer.wi < b->xfer.wn)
			wr(b, I2C_DATA, b->xfer.w[b->xfer.wi++]);
		wr(b, I2C_STAT, ST_XRDY);
	}
	if (stat & ST_ARDY) {
		wr(b, I2C_STAT, ST_ARDY);
		if (!b->xfer.reading && b->xfer.rn)
			start_read(b);
		else
			finish(b, 0);
	}
}

static void bus1_irq(void) { bus_irq(&buses[0]); }
static void bus2_irq(void) { bus_irq(&buses[1]); }
static void bus3_irq(void) { bus_irq(&buses[2]); }

/* The pins are the bootloader's and the board table's work: sda, scl
 * and func are not used here. */
int i2c_init(unsigned n, unsigned sda, unsigned scl, unsigned func, uint32_t bus_hz)
{
	static void (*const fns[3])(void) = { bus1_irq, bus2_irq, bus3_irq };
	if (n < 1 || n > 3 || !bus_hz)
		return -3;
	struct bus *b = &buses[n - 1];
	uint32_t khz = bus_hz / 1000, internal = khz > 100 ? 9600 : 4000;
	uint16_t psc = (uint16_t)(FCLK_KHZ / internal - 1), scll, sclh;
	if (khz > 100) {
		uint32_t scl = internal / khz;
		scll = (uint16_t)(scl - scl / 3 - 7);
		sclh = (uint16_t)(scl / 3 - 5);
	} else {
		scll = (uint16_t)(internal / (khz * 2) - 7);
		sclh = (uint16_t)(internal / (khz * 2) - 5);
	}
	if (b->ready && b->psc == psc && b->scll == scll && b->sclh == sclh)
		return 0;
	if (b->xfer.busy)
		return -4;
	clock_enable(CM_CORE, CORE_I2C(n), true);
	b->psc = psc;
	b->scll = scll;
	b->sclh = sclh;
	setup(b);
	irq_attach(irqs[n - 1], fns[n - 1]);
	b->ready = true;
	return 0;
}

int i2c_start(unsigned n, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn, i2c_done_fn done, void *ctx)
{
	struct bus *b = bus(n);
	if (!b || wn > MAX_WRITE || rn > MAX_READ || (!wn && !rn))
		return -3;
	uint32_t flags = irq_save();
	if (b->xfer.busy) {
		irq_restore(flags);
		return -4;
	}
	memcpy(b->xfer.w, w, wn);
	b->xfer.wn = wn;
	b->xfer.wi = 0;
	b->xfer.r = r;
	b->xfer.rn = rn;
	b->xfer.ri = 0;
	b->xfer.reading = false;
	b->xfer.done = done;
	b->xfer.ctx = ctx;
	b->xfer.addr = addr;
	b->xfer.started = time_ms();
	b->xfer.busy = true;
	/* The stop of a transfer that was refused may still be going out. */
	for (unsigned i = 0; i < 2000 && (rd(b, I2C_STAT) & ST_BB); i++)
		;
	wr(b, I2C_STAT, 0xffff);
	wr(b, I2C_BUF, BUF_CLEAR);
	wr(b, I2C_SA, addr);
	wr(b, I2C_IE, ST_AL | ST_NACK | ST_ARDY | ST_RRDY | ST_XRDY);
	if (wn) {
		wr(b, I2C_CNT, (uint16_t)wn);
		start(b, CON_EN | CON_MST | CON_TRX | (rn ? 0 : CON_STP));
	} else
		start_read(b);
	irq_restore(flags);
	return 0;
}

bool i2c_busy(unsigned n)
{
	struct bus *b = bus(n);
	return b && b->xfer.busy;
}

void i2c_set_timeout(unsigned n, uint32_t stretch_ms)
{
	struct bus *b = bus(n);
	if (b)
		b->stretch_ms = stretch_ms;
}

/* Past the usual time: still within the stretch allowance while the
 * transfer has the bus. */
static bool timed_out(const struct bus *b, uint32_t waited)
{
	if (waited <= TIMEOUT_MS)
		return false;
	if (!b->stretch_ms || waited > b->stretch_ms)
		return true;
	return !(rd(b, I2C_STAT) & ST_BB);
}

/* Clocks by hand, then a stop, for a slave that was cut off in the
 * middle of a byte and holds the data line. */
static void bus_clear(const struct bus *b)
{
	wr(b, I2C_SYSTEST, TEST_EN | TEST_IO | TEST_SCL | TEST_SDA);
	for (unsigned i = 0; i < BUS_CLEAR_CLOCKS; i++) {
		wr(b, I2C_SYSTEST, TEST_EN | TEST_IO | TEST_SDA);
		delay_us(BUS_CLEAR_HALF_US);
		wr(b, I2C_SYSTEST, TEST_EN | TEST_IO | TEST_SCL | TEST_SDA);
		delay_us(BUS_CLEAR_HALF_US);
	}
	wr(b, I2C_SYSTEST, TEST_EN | TEST_IO);
	delay_us(BUS_CLEAR_HALF_US);
	wr(b, I2C_SYSTEST, TEST_EN | TEST_IO | TEST_SCL);
	delay_us(BUS_CLEAR_HALF_US);
	wr(b, I2C_SYSTEST, TEST_EN | TEST_IO | TEST_SCL | TEST_SDA);
	delay_us(BUS_CLEAR_HALF_US);
	wr(b, I2C_SYSTEST, 0);
}

static void report(struct bus *b, unsigned n, const char *what, uint16_t stat)
{
	uint32_t now = time_ms();
	if (b->logged_t && now - b->logged_t < 1000)
		return;
	b->logged_t = now ? now : 1;
	log_printf("i2c%u: %s to %02x, status %04x\n", n, what, b->xfer.addr, stat);
}

void i2c_poll(unsigned n)
{
	struct bus *b = bus(n);
	if (!b)
		return;
	if (b->failed_stat) {
		uint16_t stat = b->failed_stat;
		b->failed_stat = 0;
		report(b, n, stat & ST_NACK ? "no answer" : "arbitration lost", stat);
	}
	if (b->xfer.busy && timed_out(b, time_ms() - b->xfer.started)) {
		uint32_t flags = irq_save();
		/* Again: an interrupt may have ended it and started another. */
		if (b->xfer.busy && timed_out(b, time_ms() - b->xfer.started)) {
			uint16_t stat = rd(b, I2C_STAT);
			setup(b);
			bus_clear(b);
			report(b, n, "timeout", stat);
			finish(b, -5);
		}
		irq_restore(flags);
	}
}

static void sync_done(int err, void *ctx) { *(volatile int *)ctx = err; }

int i2c_xfer(unsigned n, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	volatile int result = 1;
	int ret = i2c_start(n, addr, w, wn, r, rn, sync_done, (void *)&result);
	if (ret)
		return ret;
	/* With interrupts masked the controller is served from here. */
	uint32_t flags = irq_save();
	irq_restore(flags);
	while (result == 1) {
		if (flags & 0x80)
			bus_irq(&buses[n - 1]);
		i2c_poll(n);
	}
	return result;
}
