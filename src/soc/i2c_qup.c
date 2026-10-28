/* I2C master on a BLSP QUP block. The core is reset and put in I2C
 * mini-core mode before each transfer, waits for the I2C master to
 * report ready, takes its clock divider after entering RUN, and is
 * filled in PAUSE: 16-bit entries of tag and byte, two per FIFO word,
 * low half first (START+address, DATA, STOP; a read is START+address|1
 * then REC+count). Transfers fit the FIFO, so one interrupt ends each:
 * the output service flag once a write has drained, the input done flag
 * once a read has arrived (one byte per IN_DATA entry), or an error.
 * The completion callback runs in interrupt context. */
#include "asteroid.h"

#define GCC_BASE			0xfc400000u
#define GCC_BLSP1_AHB_VOTE		(GCC_BASE + 0x1484)
#define GCC_QUP_I2C_CMD_RCGR(n)		(GCC_BASE + 0x0660 + ((n) - 1) * 0x80)
#define GCC_QUP_I2C_CBCR(n)		(GCC_BASE + 0x0648 + ((n) - 1) * 0x80)

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
#define I2C_ERROR_MASK		0x38000fc
#define I2C_NACK		BIT(3)

#define TAG_START		(1u << 8)
#define TAG_DATA		(2u << 8)
#define TAG_STOP		(3u << 8)
#define TAG_REC			(4u << 8)

struct bus {
	uint32_t base, clk_ctl;
	unsigned fifo_entries;
	struct {
		volatile bool busy;
		uint8_t *r;
		unsigned rn;
		uint32_t started;
		i2c_done_fn done;
		void *ctx;
	} xfer;
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

static void qup_irq(struct bus *b)
{
	if (!b->xfer.busy) {
		writel(STATE_RESET, b->base + QUP_STATE);
		return;
	}
	uint32_t st = readl(b->base + QUP_I2C_STATUS), err = readl(b->base + QUP_ERROR_FLAGS), op = readl(b->base + QUP_OPERATIONAL);
	if ((st & I2C_ERROR_MASK) || (err & ERROR_FLAGS)) {
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

int i2c_qup_init(unsigned qup, unsigned sda, unsigned scl, unsigned func, uint32_t bus_hz)
{
	if (qup < 1 || qup > 6)
		return -3;
	struct bus *b = &buses[qup - 1];
	b->base = 0xf9923000u + (qup - 1) * 0x1000;
	writel(readl(GCC_BLSP1_AHB_VOTE) | BIT(17), GCC_BLSP1_AHB_VOTE);
	writel(0, GCC_QUP_I2C_CMD_RCGR(qup) + 4);
	writel(readl(GCC_QUP_I2C_CMD_RCGR(qup)) | 1, GCC_QUP_I2C_CMD_RCGR(qup));
	for (int i = 0; i < 1000 && (readl(GCC_QUP_I2C_CMD_RCGR(qup)) & 1); i++)
		delay_us(1);
	writel(readl(GCC_QUP_I2C_CBCR(qup)) | 1, GCC_QUP_I2C_CBCR(qup));
	for (int i = 0; i < 1000 && (readl(GCC_QUP_I2C_CBCR(qup)) & BIT(31)); i++)
		delay_us(1);
	tlmm_config(sda, func, TLMM_INPUT, TLMM_PULL_NONE, 2);
	tlmm_config(scl, func, TLMM_INPUT, TLMM_PULL_NONE, 2);
	b->clk_ctl = 3u << 8 | ((19200000 / bus_hz) / 2 - 3);
	uint32_t io = readl(b->base + QUP_IO_MODE);
	unsigned out_blk = (io & 3) ? (io & 3) * 16 : 16;
	b->fifo_entries = out_blk / 2 * (2u << ((io >> 2) & 7));
	int r = reset(b);
	gic_enable(32 + 95 + qup - 1, irqs[qup - 1]);
	log_printf("i2c: qup%u io_mode %08lx fifo %u entries, state %08lx, reset %d\n", qup, io, b->fifo_entries,
		   readl(b->base + QUP_STATE), r);
	return r;
}

static int fail(struct bus *b, const char *what)
{
	log_printf("i2c: %s: state %08lx i2c %08lx err %08lx op %08lx\n", what, readl(b->base + QUP_STATE),
		   readl(b->base + QUP_I2C_STATUS), readl(b->base + QUP_ERROR_FLAGS), readl(b->base + QUP_OPERATIONAL));
	reset(b);
	b->xfer.busy = false;
	return -1;
}

int i2c_qup_start(unsigned qup, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn, i2c_done_fn done,
		  void *ctx)
{
	struct bus *b = bus(qup);
	if (!b)
		return -3;
	uint16_t e[40];
	unsigned n = 0;
	if (b->xfer.busy)
		return -4;
	if (wn + 4 > ARRAY_SIZE(e) || rn > 256 || wn + 4 > b->fifo_entries || rn > b->fifo_entries)
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
	b->xfer.busy = true;
	b->xfer.r = r;
	b->xfer.rn = rn;
	b->xfer.done = done;
	b->xfer.ctx = ctx;
	b->xfer.started = time_ms();
	if (reset(b) || poll_state(b, 0, WAIT_MAST_GEN))
		return fail(b, "not ready");
	writel(IO_PACK_UNPACK, b->base + QUP_IO_MODE);
	writel(rn ? OP_OUT_SVC : 0, b->base + QUP_OPERATIONAL_MASK);
	if (rn)
		writel(rn, b->base + QUP_MX_READ_CNT);
	if (set_state(b, STATE_RUN))
		return fail(b, "run");
	writel(b->clk_ctl, b->base + QUP_I2C_CLK_CTL);
	dsb();
	if (set_state(b, STATE_PAUSE))
		return fail(b, "pause");
	for (unsigned i = 0; i < n; i += 2)
		writel(e[i] | (i + 1 < n ? (uint32_t)e[i + 1] << 16 : 0), b->base + QUP_OUT_FIFO);
	if (set_state(b, STATE_RUN))
		return fail(b, "start");
	return 0;
}

bool i2c_qup_busy(unsigned qup)
{
	struct bus *b = bus(qup);
	return b && b->xfer.busy;
}

void i2c_qup_poll(unsigned qup)
{
	struct bus *b = bus(qup);
	if (!b)
		return;
	if (b->xfer.busy && time_ms() - b->xfer.started > 20) {
		irq_disable();
		if (b->xfer.busy) {
			fail(b, "timeout");
			if (b->xfer.done)
				b->xfer.done(-5, b->xfer.ctx);
		}
		irq_enable();
	}
}

static void sync_done(int err, void *ctx) { *(volatile int *)ctx = err; }

int i2c_qup_xfer(unsigned qup, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	volatile int result = 1;
	int ret = i2c_qup_start(qup, addr, w, wn, r, rn, sync_done, (void *)&result);
	if (ret)
		return ret;
	while (result == 1) {
		i2c_qup_poll(qup);
		if (result == 1)
			wfi();
	}
	return result;
}
