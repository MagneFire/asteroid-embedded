/* I2C master on a BLSP QUP block. The core is reset and put in I2C
 * mini-core mode before each transfer, waits for the I2C master to
 * report ready, takes its clock divider after entering RUN, and is
 * filled in PAUSE: 16-bit entries of tag and byte, two per FIFO word,
 * low half first (START+address, DATA, STOP; a read is START+address|1
 * then REC+count). Transfers fit the FIFO, so one interrupt ends each:
 * the output service flag once a write has drained, the input done flag
 * once a read has arrived (one byte per IN_DATA entry), or an error.
 * The completion callback runs in interrupt context. A block that stops
 * taking transfers is reset through its GCC block reset, from the main
 * loop only. A bus may be given a longer timeout for a slave that holds
 * the clock while it prepares an answer: it applies while the bus is
 * active with this master on it, as the vendor kernel waits a second
 * in that case, and never to a transfer that got nowhere. A transfer
 * that still times out is followed by a bus clear with its stop, so
 * the slave sees the transaction end. */
#include "asteroid.h"

#define GCC_BLSP1_AHB_VOTE		GCC(chip->blsp_vote)
#define GCC_QUP_I2C_CMD_RCGR(n)		GCC(chip->qup_i2c_rcgr[(n) - 1])
#define GCC_QUP_I2C_CBCR(n)		GCC(chip->qup_i2c_cbcr[(n) - 1])
#define GCC_QUP_BCR(n)			GCC(chip->qup_bcr[(n) - 1])
#define IRQ_QUP(n)			(32 + 95 + (n) - 1)

#define QUP_CONFIG		0x000
#define QUP_STATE		0x004
#define QUP_IO_MODE		0x008
#define QUP_SW_RESET		0x00c
#define QUP_OPERATIONAL		0x018
#define QUP_ERROR_FLAGS		0x01c
#define QUP_ERROR_FLAGS_EN	0x020
#define QUP_MX_WR_CNT		0x100
#define QUP_OUT_FIFO		0x110
#define QUP_MX_READ_CNT		0x208
#define QUP_IN_FIFO		0x218
#define QUP_I2C_CLK_CTL		0x400
#define QUP_I2C_STATUS		0x404

#define STATE_RESET		0
#define STATE_RUN		1
#define STATE_PAUSE		3
#define STATE_MASK		3
#define STATE_VALID		BIT(2)
#define STATE_MAST_GEN		BIT(4)
#define OP_RESET		0xff0
#define OP_OUT_NOT_EMPTY	BIT(4)
#define OP_IN_NOT_EMPTY		BIT(5)
#define OP_OUT_SVC		BIT(8)
#define OP_IN_SVC		BIT(9)
#define OP_MX_INPUT_DONE	BIT(11)
#define QUP_OPERATIONAL_MASK	0x028
#define ERROR_FLAGS		0x7c
#define I2C_MINI_CORE		(2u << 8)
#define I2C_N_VAL		0xf
#define IO_PACK_UNPACK		(BIT(14) | BIT(15))
#define I2C_STATUS_RESET	0xfffffc
#define I2C_WR_BUFFER_FULL	BIT(0)
#define I2C_BUS_ACTIVE		BIT(8)
#define I2C_BUS_MASTER		BIT(9)
#define I2C_ERROR_MASK		0x38000fc
#define I2C_NACK		BIT(3)
#define I2C_SDA			BIT(26)
#define I2C_SCL			BIT(27)

#define TAG_START		(1u << 8)
#define TAG_DATA		(2u << 8)
#define TAG_STOP		(3u << 8)
#define TAG_REC			(4u << 8)

#define TIMEOUT_MS		20
#define RECOVER_MS		1000
#define LOG_MS			1000
#define BUS_CLEAR_CLOCKS	9
#define BUS_CLEAR_HALF_US	5

struct bus {
	uint32_t base, clk_ctl;
	unsigned fifo_entries;
	unsigned sda, scl, func;
	uint32_t stretch_ms;		/* allowed while the slave holds the clock, 0 for none */
	uint32_t logged_t, recover_t;
	volatile bool recover;		/* wanted by a failure in interrupt context */
	uint32_t recover_status;
	struct {
		volatile bool busy;
		uint8_t *r;
		unsigned rn;
		uint32_t started;
		i2c_done_fn done;
		void *ctx;
	} xfer;
	struct {
		const char *what;	/* a failure seen in interrupt context, logged by i2c_poll */
		uint32_t state, status, err, op;
	} pending;
};

static struct bus buses[6];

enum { WAIT_STATE, WAIT_VALID, WAIT_MAST_GEN };

static int poll_state(struct bus *b, uint32_t want, int mode)
{
	uint32_t t0 = time_us();
	while (time_us() - t0 < 5000) {
		uint32_t s = readl(b->base + QUP_STATE);
		if (!(s & STATE_VALID))
			continue;
		if (mode == WAIT_VALID || (mode == WAIT_MAST_GEN && (s & STATE_MAST_GEN)) ||
		    (mode == WAIT_STATE && (s & STATE_MASK) == want))
			return 0;
	}
	return -1;
}

static int set_state(struct bus *b, uint32_t state)
{
	if (poll_state(b, 0, WAIT_VALID))
		return -1;
	writel(state, b->base + QUP_STATE);
	return poll_state(b, state, WAIT_STATE);
}

static int reset(struct bus *b)
{
	writel(1, b->base + QUP_SW_RESET);
	if (poll_state(b, STATE_RESET, WAIT_STATE))
		return -1;
	writel(0, b->base + QUP_CONFIG);
	writel(OP_RESET, b->base + QUP_OPERATIONAL);
	writel(ERROR_FLAGS, b->base + QUP_ERROR_FLAGS_EN);
	writel(I2C_MINI_CORE | I2C_N_VAL, b->base + QUP_CONFIG);
	writel(0, b->base + QUP_I2C_CLK_CTL);
	writel(I2C_STATUS_RESET, b->base + QUP_I2C_STATUS);
	return 0;
}

static void finish(struct bus *b, int err)
{
	i2c_done_fn done = b->xfer.done;
	void *ctx = b->xfer.ctx;
	b->xfer.busy = false;
	if (done)
		done(err, ctx);
}

static void note(struct bus *b, const char *what, uint32_t state, uint32_t status, uint32_t err, uint32_t op)
{
	b->pending.what = what;
	b->pending.state = state;
	b->pending.status = status;
	b->pending.err = err;
	b->pending.op = op;
}

static void qup_irq(struct bus *b)
{
	uint32_t state = readl(b->base + QUP_STATE), st = readl(b->base + QUP_I2C_STATUS),
		 err = readl(b->base + QUP_ERROR_FLAGS), op = readl(b->base + QUP_OPERATIONAL);
	bool bad = (st & I2C_ERROR_MASK) || (err & ERROR_FLAGS);
	if (!b->xfer.busy) {
		if (bad)
			note(b, "error while idle", state, st, err, op);
		writel(STATE_RESET, b->base + QUP_STATE);
		return;
	}
	if (bad) {
		if (!(st & I2C_NACK))
			note(b, "error", state, st, err, op);
		reset(b);
		finish(b, st & I2C_NACK ? -2 : -1);
		return;
	}
	writel(op & (OP_OUT_SVC | OP_IN_SVC), b->base + QUP_OPERATIONAL);
	if (b->xfer.rn) {
		if (!(op & OP_MX_INPUT_DONE))
			return;
		uint32_t v = 0;
		for (unsigned i = 0; i < b->xfer.rn; i++) {
			if (!(i & 1))
				v = readl(b->base + QUP_IN_FIFO);
			b->xfer.r[i] = (uint8_t)((i & 1) ? v >> 16 : v);
		}
	} else if (!(op & OP_OUT_SVC) || (op & OP_OUT_NOT_EMPTY)) {
		return;
	}
	uint32_t t0 = time_us();
	while ((readl(b->base + QUP_I2C_STATUS) & (I2C_WR_BUFFER_FULL | I2C_BUS_ACTIVE)) && time_us() - t0 < 500)
		;
	writel(STATE_RESET, b->base + QUP_STATE);
	finish(b, 0);
}

#define QUP_IRQ(n) \
	static void qup##n##_irq(void) { qup_irq(&buses[(n) - 1]); }
QUP_IRQ(1)
QUP_IRQ(2)
QUP_IRQ(3)
QUP_IRQ(4)
QUP_IRQ(5)
QUP_IRQ(6)
static void (*const irqs[6])(void) = { qup1_irq, qup2_irq, qup3_irq, qup4_irq, qup5_irq, qup6_irq };

static struct bus *bus(unsigned qup)
{
	return qup >= 1 && qup <= 6 && buses[qup - 1].base ? &buses[qup - 1] : NULL;
}

static void clocks_on(unsigned qup)
{
	writel(readl(GCC_BLSP1_AHB_VOTE) | chip->blsp_vote_bit, GCC_BLSP1_AHB_VOTE);
	writel(0, GCC_QUP_I2C_CMD_RCGR(qup) + 4);
	writel(readl(GCC_QUP_I2C_CMD_RCGR(qup)) | 1, GCC_QUP_I2C_CMD_RCGR(qup));
	for (int i = 0; i < 1000 && (readl(GCC_QUP_I2C_CMD_RCGR(qup)) & 1); i++)
		delay_us(1);
	writel(readl(GCC_QUP_I2C_CBCR(qup)) | 1, GCC_QUP_I2C_CBCR(qup));
	for (int i = 0; i < 1000 && (readl(GCC_QUP_I2C_CBCR(qup)) & BIT(31)); i++)
		delay_us(1);
}

static void pins_to_qup(const struct bus *b)
{
	gpio_config(b->sda, b->func, GPIO_INPUT, GPIO_PULL_NONE, 2);
	gpio_config(b->scl, b->func, GPIO_INPUT, GPIO_PULL_NONE, 2);
}

int i2c_init(unsigned qup, unsigned sda, unsigned scl, unsigned func, uint32_t bus_hz)
{
	if (qup < 1 || qup > 6)
		return -3;
	struct bus *b = &buses[qup - 1];
	uint32_t clk_ctl = 3u << 8 | ((19200000 / bus_hz) / 2 - 3);
	/* A second device on a bus that is set up, which a reset would cut
	 * off in the middle of a transfer. */
	if (b->base && b->sda == sda && b->scl == scl && b->func == func && b->clk_ctl == clk_ctl)
		return 0;
	b->base = chip->qup1 + (qup - 1) * 0x1000;
	b->sda = sda;
	b->scl = scl;
	b->func = func;
	clocks_on(qup);
	pins_to_qup(b);
	b->clk_ctl = clk_ctl;
	uint32_t io = readl(b->base + QUP_IO_MODE);
	unsigned out_blk = (io & 3) ? (io & 3) * 16 : 16;
	b->fifo_entries = out_blk / 2 * (2u << ((io >> 2) & 7));
	int r = reset(b);
	irq_attach(IRQ_QUP(qup), irqs[qup - 1]);
	log_printf("i2c: qup%u io_mode %08lx fifo %u entries, state %08lx, reset %d\n", qup, io, b->fifo_entries,
		   readl(b->base + QUP_STATE), r);
	return r;
}

static unsigned qup_of(const struct bus *b)
{
	return (unsigned)(b - buses) + 1;
}

static void report(struct bus *b, const char *what, const char *where, uint32_t state, uint32_t status, uint32_t err,
		   uint32_t op)
{
	uint32_t now = time_ms();
	unsigned q = qup_of(b);
	if (now - b->logged_t < LOG_MS)
		return;
	b->logged_t = now;
	log_printf("i2c: qup%u %s%s: state %08lx i2c %08lx err %08lx op %08lx, cbcr %08lx rcgr %08lx ahb %08lx\n", q, what, where,
		   state, status, err, op, readl(GCC_QUP_I2C_CBCR(q)), readl(GCC_QUP_I2C_CMD_RCGR(q)),
		   readl(GCC_BLSP1_AHB_VOTE));
}

/* Nine clocks free a slave stuck in a byte, then a stop. The lines are
 * only ever pulled low, the bus's own pull-ups raise them. */
static void bus_clear(const struct bus *b)
{
	gpio_set(b->sda, 0);
	gpio_set(b->scl, 0);
	gpio_config(b->sda, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	gpio_config(b->scl, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	for (unsigned i = 0; i < BUS_CLEAR_CLOCKS; i++) {
		gpio_config(b->scl, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
		delay_us(BUS_CLEAR_HALF_US);
		gpio_config(b->scl, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
		delay_us(BUS_CLEAR_HALF_US);
	}
	gpio_config(b->scl, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	delay_us(BUS_CLEAR_HALF_US);
	gpio_config(b->sda, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	delay_us(BUS_CLEAR_HALF_US);
	gpio_config(b->scl, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	delay_us(BUS_CLEAR_HALF_US);
	gpio_config(b->sda, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	delay_us(BUS_CLEAR_HALF_US);
}

static bool recover_due(const struct bus *b)
{
	return !b->recover_t || time_ms() - b->recover_t >= RECOVER_MS;
}

/* Not in interrupt context, and with the bus held busy so nothing starts
 * on it. status is from before any reset. */
static void recover(struct bus *b, uint32_t status)
{
	unsigned q = qup_of(b);
	b->recover_t = time_ms();
	b->recover = false;
	if ((status & I2C_BUS_ACTIVE) || (status & (I2C_SDA | I2C_SCL)) != (I2C_SDA | I2C_SCL))
		bus_clear(b);
	writel(1, GCC_QUP_BCR(q));
	delay_ms(1);
	writel(0, GCC_QUP_BCR(q));
	delay_us(1);
	clocks_on(q);
	pins_to_qup(b);
	bool ok = !reset(b) && !poll_state(b, 0, WAIT_MAST_GEN) && !set_state(b, STATE_RUN);
	reset(b);
	log_printf("i2c: qup%u %s\n", q, ok ? "recovered" : "not recovered");
}

static int fail(struct bus *b, const char *what, bool clear)
{
	uint32_t state = readl(b->base + QUP_STATE), status = readl(b->base + QUP_I2C_STATUS),
		 err = readl(b->base + QUP_ERROR_FLAGS), op = readl(b->base + QUP_OPERATIONAL);
	if (in_irq()) {
		note(b, what, state, status, err, op);
		b->recover_status = status;
		b->recover = true;
		reset(b);
	} else {
		report(b, what, "", state, status, err, op);
		if (clear || recover_due(b))
			recover(b, status);
		else
			reset(b);
	}
	b->xfer.busy = false;
	irq_unmask(IRQ_QUP(qup_of(b)));
	return -1;
}

int i2c_start(unsigned qup, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn, i2c_done_fn done,
		  void *ctx)
{
	struct bus *b = bus(qup);
	if (!b)
		return -3;
	uint16_t e[40];
	unsigned n = 0;
	if (wn > ARRAY_SIZE(e) - 4 || rn > 256 || b->fifo_entries < 4 || wn > b->fifo_entries - 4 ||
	    rn > b->fifo_entries)
		return -3;
	if (wn) {
		e[n++] = TAG_START | (uint16_t)(addr << 1);
		for (unsigned i = 0; i < wn; i++)
			e[n++] = (uint16_t)(((i + 1 < wn || rn) ? TAG_DATA : TAG_STOP) | w[i]);
	}
	if (rn) {
		e[n++] = TAG_START | (uint16_t)(addr << 1 | 1);
		e[n++] = (uint16_t)(TAG_REC | (rn & 0xff));
	}
	/* Masked until the FIFO is in: setup raises flags qup_irq would act on. */
	uint32_t flags = irq_save();
	if (b->xfer.busy) {
		irq_restore(flags);
		return -4;
	}
	b->xfer.r = r;
	b->xfer.rn = rn;
	b->xfer.done = done;
	b->xfer.ctx = ctx;
	b->xfer.started = time_ms();
	irq_mask(IRQ_QUP(qup));
	dsb();
	b->xfer.busy = true;
	irq_restore(flags);
	if (reset(b) || poll_state(b, 0, WAIT_MAST_GEN))
		return fail(b, "not ready", false);
	writel(IO_PACK_UNPACK, b->base + QUP_IO_MODE);
	writel(rn ? OP_OUT_SVC : 0, b->base + QUP_OPERATIONAL_MASK);
	if (rn)
		writel(rn, b->base + QUP_MX_READ_CNT);
	if (set_state(b, STATE_RUN))
		return fail(b, "run", false);
	writel(b->clk_ctl, b->base + QUP_I2C_CLK_CTL);
	dsb();
	if (set_state(b, STATE_PAUSE))
		return fail(b, "pause", false);
	for (unsigned i = 0; i < n; i += 2)
		writel(e[i] | (i + 1 < n ? (uint32_t)e[i + 1] << 16 : 0), b->base + QUP_OUT_FIFO);
	if (set_state(b, STATE_RUN))
		return fail(b, "start", false);
	irq_unmask(IRQ_QUP(qup));
	return 0;
}

bool i2c_busy(unsigned qup)
{
	struct bus *b = bus(qup);
	return b && b->xfer.busy;
}

void i2c_set_timeout(unsigned qup, uint32_t stretch_ms)
{
	struct bus *b = bus(qup);
	if (b)
		b->stretch_ms = stretch_ms;
}

/* Past the usual time: still within the stretch allowance while the
 * slave holds the clock of a transfer this master has on the bus. */
static bool timed_out(const struct bus *b, uint32_t waited)
{
	if (waited <= TIMEOUT_MS)
		return false;
	if (!b->stretch_ms || waited > b->stretch_ms)
		return true;
	uint32_t status = readl(b->base + QUP_I2C_STATUS);
	return (status & (I2C_BUS_ACTIVE | I2C_BUS_MASTER)) != (I2C_BUS_ACTIVE | I2C_BUS_MASTER);
}

void i2c_poll(unsigned qup)
{
	struct bus *b = bus(qup);
	if (!b)
		return;
	if (b->pending.what) {
		uint32_t flags = irq_save();
		const char *what = b->pending.what;
		uint32_t state = b->pending.state, status = b->pending.status, err = b->pending.err, op = b->pending.op;
		b->pending.what = NULL;
		irq_restore(flags);
		report(b, what, " (in irq)", state, status, err, op);
	}
	if (b->recover && recover_due(b) && !in_irq()) {
		uint32_t flags = irq_save();
		bool idle = !b->xfer.busy;
		if (idle) {
			irq_mask(IRQ_QUP(qup));
			b->xfer.busy = true;
		}
		irq_restore(flags);
		if (idle) {
			recover(b, b->recover_status);
			b->xfer.busy = false;
			irq_unmask(IRQ_QUP(qup));
		}
	}
	if (b->xfer.busy && timed_out(b, time_ms() - b->xfer.started)) {
		uint32_t flags = irq_save();
		/* Again: an interrupt may have ended it and started another. */
		if (b->xfer.busy && timed_out(b, time_ms() - b->xfer.started)) {
			fail(b, "timeout", true);
			if (b->xfer.done)
				b->xfer.done(-5, b->xfer.ctx);
		}
		irq_restore(flags);
	}
}

static void sync_done(int err, void *ctx) { *(volatile int *)ctx = err; }

/* A transfer started from an interrupt, on a bus another device shares,
 * is waited for first. */
#define BUSY_WAIT_MS	50

int i2c_xfer(unsigned qup, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	volatile int result = 1;
	uint32_t t0 = time_ms();
	int ret;
	while ((ret = i2c_start(qup, addr, w, wn, r, rn, sync_done, (void *)&result)) == -4 && time_ms() - t0 < BUSY_WAIT_MS)
		i2c_poll(qup);
	if (ret)
		return ret;
	while (result == 1)
		i2c_poll(qup);
	return result;
}
