/* SPI master on a BLSP QUP block, polled. The core is reset and put in
 * SPI mini-core mode (8-bit words) before each chunk, in FIFO mode with
 * the FIFO counts and no packing: one FIFO word per byte, sent from its
 * top byte and received in its low byte. Every transfer is full duplex.
 * A chunk is what fits the FIFO: it is filled in PAUSE,
 * the clock runs on RUN, and the received bytes are read as they arrive.
 * Chip select is not the block's: the caller drives it as a pin, so it
 * stays low across the chunks of one transfer. */
#include "asteroid.h"

#define GCC_BLSP1_AHB_VOTE		GCC(chip->blsp_vote)
#define GCC_APCS_GPLL_ENA_VOTE		GCC(chip->gpll0_vote)
#define GCC_GPLL0_STATUS		GCC(chip->gpll0_status)
#define GCC_QUP_SPI_CMD_RCGR(n)		GCC(chip->qup_spi_rcgr[(n) - 1])
#define GCC_QUP_SPI_CBCR(n)		GCC(chip->qup_spi_cbcr[(n) - 1])

#define RCG_CFG			0x04
#define RCG_M			0x08
#define RCG_N			0x0c
#define RCG_D			0x10
#define RCG_UPDATE		BIT(0)
#define RCG_SRC_GPLL0		(1u << 8)
#define RCG_MODE_DUAL_EDGE	(2u << 12)
#define CBCR_ENABLE		BIT(0)
#define CBCR_OFF		BIT(31)

#define QUP_CONFIG		0x000
#define QUP_STATE		0x004
#define QUP_IO_MODE		0x008
#define QUP_SW_RESET		0x00c
#define QUP_OPERATIONAL		0x018
#define QUP_ERROR_FLAGS		0x01c
#define QUP_ERROR_FLAGS_EN	0x020
#define QUP_OPERATIONAL_MASK	0x028
#define QUP_MX_OUTPUT_CNT	0x100
#define QUP_OUT_FIFO		0x110
#define QUP_MX_WRITE_CNT	0x150
#define QUP_MX_INPUT_CNT	0x200
#define QUP_MX_READ_CNT		0x208
#define QUP_IN_FIFO		0x218
#define SPI_CONFIG		0x300
#define SPI_IO_CONTROL		0x304
#define SPI_ERROR_FLAGS		0x308
#define SPI_ERROR_FLAGS_EN	0x30c

#define STATE_RESET		0
#define STATE_RUN		1
#define STATE_PAUSE		3
#define STATE_MASK		3
#define STATE_VALID		BIT(2)
#define OP_RESET		0xff0
#define OP_IN_NOT_EMPTY		BIT(5)
#define QUP_ERRORS		0x3c
#define SPI_ERRORS		0x3
#define SPI_MINI_CORE		(1u << 8)
#define SPI_N_VAL		7
#define SPI_INPUT_FIRST		BIT(9)
#define SPI_IO_NO_TRI_STATE	BIT(0)
#define SPI_IO_CLK_IDLE_HIGH	BIT(10)

#define MIN_CHUNK		4
#define STATE_TIMEOUT_US	5000
#define CHUNK_SLACK_US		5000

struct spi {
	uint32_t base, config, control;
	uint32_t hz;
	unsigned chunk;			/* bytes that fit the FIFOs */
};

static struct spi spis[6];

/* GPLL0 is 800 MHz; m/n encode the fraction as the root clock wants it. */
static const struct {
	uint32_t hz;
	uint32_t cfg;
	uint16_t m, n;
} rates[] = {
	{ 960000, 19, 1, 2 },
	{ 4800000, 7, 0, 0 },
	{ 9600000, 3, 0, 0 },
	{ 16000000, RCG_SRC_GPLL0 | 19, 1, 5 },
	{ 19200000, 1, 0, 0 },
	{ 25000000, RCG_SRC_GPLL0 | 31, 1, 2 },
	{ 50000000, RCG_SRC_GPLL0 | 31, 0, 0 },
};

static int poll_state(struct spi *s, uint32_t want, bool any)
{
	uint32_t t0 = time_us();
	while (time_us() - t0 < STATE_TIMEOUT_US) {
		uint32_t v = readl(s->base + QUP_STATE);
		if ((v & STATE_VALID) && (any || (v & STATE_MASK) == want))
			return 0;
	}
	return -1;
}

static int set_state(struct spi *s, uint32_t state)
{
	if (poll_state(s, 0, true))
		return -1;
	writel(state, s->base + QUP_STATE);
	return poll_state(s, state, false);
}

static int reset(struct spi *s)
{
	writel(1, s->base + QUP_SW_RESET);
	if (poll_state(s, STATE_RESET, false))
		return -1;
	writel(0, s->base + QUP_CONFIG);
	writel(OP_RESET, s->base + QUP_OPERATIONAL);
	writel(QUP_ERRORS, s->base + QUP_ERROR_FLAGS_EN);
	writel(SPI_ERRORS, s->base + SPI_ERROR_FLAGS_EN);
	writel(s->control, s->base + SPI_IO_CONTROL);
	writel(s->config, s->base + SPI_CONFIG);
	writel(0, s->base + QUP_OPERATIONAL_MASK);
	return 0;
}

static bool set_clock(unsigned qup, uint32_t hz, uint32_t *got)
{
	unsigned pick = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(rates); i++)
		if (rates[i].hz <= hz)
			pick = i;
	uint32_t rcg = GCC_QUP_SPI_CMD_RCGR(qup);
	writel(readl(GCC_APCS_GPLL_ENA_VOTE) | BIT(0), GCC_APCS_GPLL_ENA_VOTE);
	for (int i = 0; i < 1000 && !(readl(GCC_GPLL0_STATUS) & chip->gpll0_ready); i++)
		delay_us(1);
	writel(readl(GCC_BLSP1_AHB_VOTE) | chip->blsp_vote_bit, GCC_BLSP1_AHB_VOTE);
	writel(rates[pick].m, rcg + RCG_M);
	writel(rates[pick].n ? ~(rates[pick].n - rates[pick].m) : 0, rcg + RCG_N);
	writel(rates[pick].n ? ~rates[pick].n : 0, rcg + RCG_D);
	writel(rates[pick].cfg | (rates[pick].n ? RCG_MODE_DUAL_EDGE : 0), rcg + RCG_CFG);
	writel(readl(rcg) | RCG_UPDATE, rcg);
	for (int i = 0; i < 1000 && (readl(rcg) & RCG_UPDATE); i++)
		delay_us(1);
	if (readl(rcg) & RCG_UPDATE)
		return false;
	writel(readl(GCC_QUP_SPI_CBCR(qup)) | CBCR_ENABLE, GCC_QUP_SPI_CBCR(qup));
	for (int i = 0; i < 1000 && (readl(GCC_QUP_SPI_CBCR(qup)) & CBCR_OFF); i++)
		delay_us(1);
	*got = rates[pick].hz;
	return !(readl(GCC_QUP_SPI_CBCR(qup)) & CBCR_OFF);
}

int spi_init(unsigned qup, const uint8_t pins[3], unsigned func, unsigned mode, uint32_t hz)
{
	if (qup < 1 || qup > 6)
		return -3;
	struct spi *s = &spis[qup - 1];
	s->base = chip->qup1 + (qup - 1) * 0x1000;
	if (!set_clock(qup, hz, &s->hz)) {
		log_printf("spi: qup%u clock failed (cbcr %08lx rcgr %08lx)\n", qup, readl(GCC_QUP_SPI_CBCR(qup)),
			   readl(GCC_QUP_SPI_CMD_RCGR(qup)));
		s->base = 0;
		return -1;
	}
	gpio_config(pins[0], func, GPIO_INPUT, GPIO_PULL_NONE, 12);
	gpio_config(pins[1], func, GPIO_INPUT, GPIO_PULL_NONE, 12);
	gpio_config(pins[2], func, GPIO_INPUT, GPIO_PULL_NONE, 12);
	s->control = SPI_IO_NO_TRI_STATE | ((mode & 2) ? SPI_IO_CLK_IDLE_HIGH : 0);
	s->config = (mode & 1) ? 0 : SPI_INPUT_FIRST;
	uint32_t io = readl(s->base + QUP_IO_MODE);
	unsigned blk = (io & 3) ? (io & 3) * 16 : 4;
	unsigned out = blk * (2u << ((io >> 2) & 7));
	unsigned in_blk = ((io >> 5) & 3) ? ((io >> 5) & 3) * 16 : 4;
	unsigned in = in_blk * (2u << ((io >> 7) & 7));
	s->chunk = (out < in ? out : in) / 4;
	if (s->chunk < MIN_CHUNK)
		s->chunk = MIN_CHUNK;
	int r = reset(s);
	log_printf("spi: qup%u io_mode %08lx chunk %u bytes, %lu Hz, reset %d\n", qup, io, s->chunk, s->hz, r);
	if (r)
		s->base = 0;
	return r;
}

static int fail(struct spi *s, const char *what)
{
	log_printf("spi: %s: state %08lx op %08lx err %08lx spi_err %08lx\n", what, readl(s->base + QUP_STATE),
		   readl(s->base + QUP_OPERATIONAL), readl(s->base + QUP_ERROR_FLAGS),
		   readl(s->base + SPI_ERROR_FLAGS));
	reset(s);
	return -1;
}

static int chunk(struct spi *s, const uint8_t *tx, uint8_t *rx, unsigned n)
{
	if (reset(s))
		return fail(s, "not ready");
	writel(SPI_MINI_CORE | SPI_N_VAL, s->base + QUP_CONFIG);
	writel(0, s->base + QUP_IO_MODE);
	writel(0, s->base + QUP_MX_OUTPUT_CNT);
	writel(0, s->base + QUP_MX_INPUT_CNT);
	writel(n, s->base + QUP_MX_WRITE_CNT);
	writel(n, s->base + QUP_MX_READ_CNT);
	if (set_state(s, STATE_RUN) || set_state(s, STATE_PAUSE))
		return fail(s, "pause");
	for (unsigned k = 0; k < n; k++)
		writel((uint32_t)(tx ? tx[k] : 0xff) << 24, s->base + QUP_OUT_FIFO);
	if (set_state(s, STATE_RUN))
		return fail(s, "run");
	uint32_t t0 = time_us(), limit = CHUNK_SLACK_US + n * 8000u / (s->hz / 1000) * 2;
	unsigned got = 0;
	while (got < n) {
		uint32_t op = readl(s->base + QUP_OPERATIONAL);
		if (readl(s->base + QUP_ERROR_FLAGS) & QUP_ERRORS || readl(s->base + SPI_ERROR_FLAGS) & SPI_ERRORS)
			return fail(s, "error");
		if (op & OP_IN_NOT_EMPTY) {
			uint8_t v = (uint8_t)readl(s->base + QUP_IN_FIFO);
			if (rx)
				rx[got] = v;
			got++;
		} else if (time_us() - t0 > limit) {
			return fail(s, "timeout");
		}
	}
	return set_state(s, STATE_RESET) ? fail(s, "idle") : 0;
}

int spi_xfer(unsigned qup, const uint8_t *tx, uint8_t *rx, unsigned n)
{
	if (qup < 1 || qup > 6 || !spis[qup - 1].base)
		return -3;
	struct spi *s = &spis[qup - 1];
	for (unsigned done = 0; done < n;) {
		unsigned c = n - done < s->chunk ? n - done : s->chunk;
		int r = chunk(s, tx ? tx + done : NULL, rx ? rx + done : NULL, c);
		if (r)
			return r;
		done += c;
	}
	return 0;
}
