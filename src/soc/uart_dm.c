/* High-speed UART on a BLSP UARTDM core (v1.4), FIFO mode, no BAM. The
 * baud clock comes from the block's RCG: 7.3728 MHz with the CSR
 * dividing it for rates up to 460800, sixteen times the baud rate above
 * that. RTS/CTS are handled by the core (auto RFR at an RX FIFO level,
 * transmit held while CTS is deasserted). Receive is interrupt driven:
 * the RX level interrupt drains whole words, the stale interrupt the
 * rest of a transfer (four bytes per FIFO word, RX_TOTAL_SNAP counts the
 * bytes), into a ring that the main loop reads. Transmit is polled. Each
 * core has its own state, so several UARTs can run at once. */
#include "asteroid.h"

#define GCC_BASE			0xfc400000u
#define GCC_GPLL0_STATUS		(GCC_BASE + 0x001c)
#define GCC_APCS_GPLL_ENA_VOTE		(GCC_BASE + 0x1480)
#define GCC_BLSP1_AHB_VOTE		(GCC_BASE + 0x1484)
#define GCC_UART_APPS_CBCR(n)		(GCC_BASE + 0x0684 + ((n) - 1) * 0x80)
#define GCC_UART_APPS_CMD_RCGR(n)	(GCC_BASE + 0x068c + ((n) - 1) * 0x80)
#define RCG_CFG				0x04
#define RCG_M				0x08
#define RCG_N				0x0c
#define RCG_D				0x10
#define RCG_UPDATE			BIT(0)
#define RCG_SRC_GPLL0			(1u << 8)
#define RCG_MODE_DUAL_EDGE		(2u << 12)
#define CBCR_ENABLE			BIT(0)
#define CBCR_OFF			BIT(31)

#define UART_DM_MR1		0x000
#define UART_DM_MR2		0x004
#define UART_DM_IPR		0x018
#define UART_DM_TFWR		0x01c
#define UART_DM_RFWR		0x020
#define UART_DM_DMRX		0x034
#define UART_DM_DMEN		0x03c
#define UART_DM_NCF_TX		0x040
#define UART_DM_BADR		0x044
#define UART_DM_CSR		0x0a0
#define UART_DM_SR		0x0a4
#define UART_DM_CR		0x0a8
#define UART_DM_MISR		0x0ac
#define UART_DM_IMR		0x0b0
#define UART_DM_ISR		0x0b4
#define UART_DM_RX_TOTAL_SNAP	0x0bc
#define UART_DM_TF		0x100
#define UART_DM_RF		0x140

#define MR1_RFR_LEVEL(n)	(((n) & 0x3f) | (((n) << 2) & 0xffffff00))
#define MR1_CTS_CTL		BIT(6)
#define MR1_RX_RDY_CTL		BIT(7)
#define MR2_8N1			(3u << 4 | 1u << 2)
#define MR2_ERROR_MODE		BIT(6)
#define MR2_RX_ZERO_CHAR_OFF	BIT(8)
#define MR2_RX_ERROR_CHAR_OFF	BIT(9)
#define SR_RXRDY		BIT(0)
#define SR_TXRDY		BIT(2)
#define SR_TXEMT		BIT(3)
#define SR_OVERRUN		BIT(4)
#define ISR_RXSTALE		BIT(3)
#define ISR_RXLEV		BIT(4)
#define ISR_TX_READY		BIT(7)
#define CR_RX_EN		BIT(0)
#define CR_RX_DISABLE		BIT(1)
#define CR_TX_EN		BIT(2)
#define CR_TX_DISABLE		BIT(3)
#define CR_RESET_RX		0x010
#define CR_RESET_TX		0x020
#define CR_RESET_ERROR		0x030
#define CR_RESET_BREAK		0x040
#define CR_RESET_CTS		0x070
#define CR_RESET_STALE		0x080
#define CR_SET_RFR		0x0d0
#define CR_RESET_RFR		0x0e0
#define CR_PROTECTION_EN	0x100
#define CR_CLEAR_TX_READY	0x300
#define CR_STALE_ENABLE		0x500

#define RX_WATERMARK_WORDS	16
#define RFR_LEVEL		48
#define FIFO_SPLIT_WORDS	64
#define STALE_CHARS		31
#define TX_TIMEOUT_US		100000
#define RING_SIZE		4096

struct baud {
	uint32_t baud, clk_cfg, m, n;
	uint8_t csr;
};

static const struct baud bauds[] = {
	{ 115200, RCG_SRC_GPLL0 | 1, 192, 15625, 0xcc },
	{ 230400, RCG_SRC_GPLL0 | 1, 192, 15625, 0xee },
	{ 460800, RCG_SRC_GPLL0 | 1, 192, 15625, 0xff },
	{ 921600, RCG_SRC_GPLL0 | 1, 384, 15625, 0xff },
	{ 1000000, RCG_SRC_GPLL0 | 9, 2, 15, 0xff },
	{ 1500000, RCG_SRC_GPLL0 | 9, 1, 5, 0xff },
	{ 2000000, RCG_SRC_GPLL0 | 1, 4, 75, 0xff },
	{ 3000000, RCG_SRC_GPLL0 | 24, 0, 0, 0xff },
};

struct uart {
	uint32_t base;
	unsigned port, irq, pins;
	bool flow;
	uint8_t ring[RING_SIZE];
	volatile uint32_t ring_head, ring_tail;
	uint32_t rx_taken, rx_bytes, tx_bytes, dropped, overruns;
};

static struct uart uarts[6];

static struct uart *get(unsigned uart)
{
	return uart >= 1 && uart <= ARRAY_SIZE(uarts) && uarts[uart - 1].base ? &uarts[uart - 1] : NULL;
}

static void put(struct uart *u, uint32_t w, unsigned n)
{
	for (unsigned i = 0; i < n; i++, w >>= 8) {
		if (u->ring_head - u->ring_tail >= RING_SIZE) {
			u->dropped++;
			continue;
		}
		u->ring[u->ring_head % RING_SIZE] = (uint8_t)w;
		barrier();
		u->ring_head++;
	}
	u->rx_bytes += n;
}

static void rx_restart(struct uart *u)
{
	u->rx_taken = 0;
	writel(0xffffff, u->base + UART_DM_DMRX);
	writel(CR_STALE_ENABLE, u->base + UART_DM_CR);
}

static void uart_irq(struct uart *u)
{
	uint32_t misr = readl(u->base + UART_DM_MISR);
	if (readl(u->base + UART_DM_SR) & SR_OVERRUN) {
		u->overruns++;
		writel(CR_RESET_ERROR, u->base + UART_DM_CR);
	}
	if (misr & ISR_RXSTALE) {
		uint32_t total = readl(u->base + UART_DM_RX_TOTAL_SNAP);
		uint32_t left = total > u->rx_taken ? total - u->rx_taken : 0;
		while (left && (readl(u->base + UART_DM_SR) & SR_RXRDY)) {
			unsigned k = left < 4 ? left : 4;
			put(u, readl(u->base + UART_DM_RF), k);
			left -= k;
		}
		writel(CR_RESET_STALE, u->base + UART_DM_CR);
		rx_restart(u);
	} else if (misr & ISR_RXLEV) {
		for (unsigned i = 0; i < RX_WATERMARK_WORDS && (readl(u->base + UART_DM_SR) & SR_RXRDY); i++) {
			put(u, readl(u->base + UART_DM_RF), 4);
			u->rx_taken += 4;
		}
	}
}

static void irq1(void) { uart_irq(&uarts[0]); }
static void irq2(void) { uart_irq(&uarts[1]); }
static void irq3(void) { uart_irq(&uarts[2]); }
static void irq4(void) { uart_irq(&uarts[3]); }
static void irq5(void) { uart_irq(&uarts[4]); }
static void irq6(void) { uart_irq(&uarts[5]); }
static void (*const irqs[])(void) = { irq1, irq2, irq3, irq4, irq5, irq6 };

static const struct baud *find_baud(uint32_t baud)
{
	for (unsigned i = 0; i < ARRAY_SIZE(bauds); i++)
		if (bauds[i].baud == baud)
			return &bauds[i];
	return NULL;
}

static bool set_clock(struct uart *u, const struct baud *b)
{
	uint32_t rcg = GCC_UART_APPS_CMD_RCGR(u->port);
	writel(b->m, rcg + RCG_M);
	writel(b->n ? ~(b->n - b->m) : 0, rcg + RCG_N);
	writel(b->n ? ~b->n : 0, rcg + RCG_D);
	writel(b->clk_cfg | (b->n ? RCG_MODE_DUAL_EDGE : 0), rcg + RCG_CFG);
	writel(readl(rcg) | RCG_UPDATE, rcg);
	for (int i = 0; i < 1000 && (readl(rcg) & RCG_UPDATE); i++)
		delay_us(1);
	return !(readl(rcg) & RCG_UPDATE);
}

static void start(struct uart *u, const struct baud *b)
{
	writel(0, u->base + UART_DM_IMR);
	writel(b->csr, u->base + UART_DM_CSR);
	writel(STALE_CHARS, u->base + UART_DM_IPR);
	writel(RX_WATERMARK_WORDS, u->base + UART_DM_RFWR);
	writel(0, u->base + UART_DM_TFWR);
	writel(FIFO_SPLIT_WORDS, u->base + UART_DM_BADR);
	writel(MR1_RFR_LEVEL(RFR_LEVEL) | (u->flow ? MR1_CTS_CTL | MR1_RX_RDY_CTL : 0), u->base + UART_DM_MR1);
	writel(MR2_8N1 | MR2_ERROR_MODE | MR2_RX_ZERO_CHAR_OFF | MR2_RX_ERROR_CHAR_OFF, u->base + UART_DM_MR2);
	writel(0, u->base + UART_DM_DMEN);
	writel(CR_PROTECTION_EN, u->base + UART_DM_CR);
	writel(CR_RESET_RX, u->base + UART_DM_CR);
	writel(CR_RESET_TX, u->base + UART_DM_CR);
	writel(CR_RESET_ERROR, u->base + UART_DM_CR);
	writel(CR_RESET_BREAK, u->base + UART_DM_CR);
	writel(CR_RESET_STALE, u->base + UART_DM_CR);
	writel(CR_RESET_CTS, u->base + UART_DM_CR);
	writel(CR_SET_RFR, u->base + UART_DM_CR);
	writel(CR_RX_EN | CR_TX_EN, u->base + UART_DM_CR);
	rx_restart(u);
	writel(ISR_RXLEV | ISR_RXSTALE, u->base + UART_DM_IMR);
}

static int wait_tx_ready(struct uart *u)
{
	if (readl(u->base + UART_DM_SR) & SR_TXEMT)
		return 0;
	uint32_t t0 = time_us();
	while (!(readl(u->base + UART_DM_ISR) & ISR_TX_READY)) {
		if (readl(u->base + UART_DM_SR) & SR_TXEMT)
			break;
		if (time_us() - t0 > TX_TIMEOUT_US)
			return -1;
	}
	return 0;
}

int uart_dm_init(unsigned uart, unsigned first_gpio, unsigned func, uint32_t baud, bool flow)
{
	const struct baud *b = find_baud(baud);
	if (!b || uart < 1 || uart > ARRAY_SIZE(uarts))
		return -1;
	struct uart *u = &uarts[uart - 1];
	u->port = uart;
	u->pins = first_gpio;
	u->flow = flow;
	u->irq = 32 + 107 + uart - 1;
	writel(readl(GCC_APCS_GPLL_ENA_VOTE) | BIT(0), GCC_APCS_GPLL_ENA_VOTE);
	for (int i = 0; i < 1000 && !(readl(GCC_GPLL0_STATUS) & BIT(17)); i++)
		delay_us(1);
	writel(readl(GCC_BLSP1_AHB_VOTE) | BIT(17), GCC_BLSP1_AHB_VOTE);
	if (!set_clock(u, b)) {
		log_printf("uart%u: baud clock rcg stuck (%08lx)\n", uart, readl(GCC_UART_APPS_CMD_RCGR(uart)));
		return -1;
	}
	writel(readl(GCC_UART_APPS_CBCR(uart)) | CBCR_ENABLE, GCC_UART_APPS_CBCR(uart));
	for (int i = 0; i < 1000 && (readl(GCC_UART_APPS_CBCR(uart)) & CBCR_OFF); i++)
		delay_us(1);
	if (readl(GCC_UART_APPS_CBCR(uart)) & CBCR_OFF) {
		log_printf("uart%u: clock stays off (%08lx)\n", uart, readl(GCC_UART_APPS_CBCR(uart)));
		return -1;
	}
	for (unsigned i = 0; i < 4; i++)
		tlmm_config(first_gpio + i, func, TLMM_INPUT, TLMM_PULL_NONE, 8);
	u->ring_head = u->ring_tail = 0;
	u->rx_bytes = u->tx_bytes = u->dropped = u->overruns = 0;
	u->base = 0xf991d000u + (uart - 1) * 0x1000;
	start(u, b);
	gic_enable(u->irq, irqs[uart - 1]);
	log_printf("uart%u: %lu baud, sr %08lx isr %08lx badr %lu\n", uart, baud, readl(u->base + UART_DM_SR),
		   readl(u->base + UART_DM_ISR), readl(u->base + UART_DM_BADR));
	return 0;
}

int uart_dm_set_baud(unsigned uart, uint32_t baud)
{
	struct uart *u = get(uart);
	const struct baud *b = find_baud(baud);
	if (!b || !u)
		return -1;
	uint32_t t0 = time_us();
	while (!(readl(u->base + UART_DM_SR) & SR_TXEMT) && time_us() - t0 < TX_TIMEOUT_US)
		;
	gic_mask(u->irq);
	writel(CR_RX_DISABLE | CR_TX_DISABLE, u->base + UART_DM_CR);
	dsb();
	bool ok = set_clock(u, b);
	start(u, b);
	gic_unmask(u->irq);
	return ok ? 0 : -1;
}

int uart_dm_write(unsigned uart, const uint8_t *p, unsigned n)
{
	struct uart *u = get(uart);
	if (!u || !n)
		return -1;
	if (wait_tx_ready(u))
		return -2;
	writel(n, u->base + UART_DM_NCF_TX);
	writel(CR_CLEAR_TX_READY, u->base + UART_DM_CR);
	for (unsigned i = 0; i < n; i += 4) {
		uint32_t w = 0, t0 = time_us();
		for (unsigned j = 0; j < 4 && i + j < n; j++)
			w |= (uint32_t)p[i + j] << (8 * j);
		while (!(readl(u->base + UART_DM_SR) & SR_TXRDY))
			if (time_us() - t0 > TX_TIMEOUT_US)
				return -3;
		writel(w, u->base + UART_DM_TF);
	}
	u->tx_bytes += n;
	return 0;
}

unsigned uart_dm_read(unsigned uart, uint8_t *p, unsigned n)
{
	struct uart *u = get(uart);
	unsigned k = 0;
	if (!u)
		return 0;
	while (k < n && u->ring_tail != u->ring_head) {
		barrier();
		p[k++] = u->ring[u->ring_tail % RING_SIZE];
		barrier();
		u->ring_tail++;
	}
	return k;
}

bool uart_dm_cts(unsigned uart)
{
	struct uart *u = get(uart);
	return u && (readl(u->base + UART_DM_ISR) & BIT(6));
}

void uart_dm_stop(unsigned uart)
{
	struct uart *u = get(uart);
	if (!u)
		return;
	gic_disable(u->irq);
	writel(0, u->base + UART_DM_IMR);
	writel(CR_RX_DISABLE | CR_TX_DISABLE, u->base + UART_DM_CR);
	writel(readl(GCC_UART_APPS_CBCR(u->port)) & ~CBCR_ENABLE, GCC_UART_APPS_CBCR(u->port));
	for (unsigned i = 0; i < 4; i++)
		tlmm_config(u->pins + i, 0, TLMM_INPUT, TLMM_PULL_DOWN, 2);
	u->base = 0;
}

void uart_dm_stats(unsigned uart, uint32_t *rx, uint32_t *tx, uint32_t *errors)
{
	struct uart *u = &uarts[uart - 1];
	*rx = u->rx_bytes;
	*tx = u->tx_bytes;
	*errors = u->dropped + u->overruns;
}

bool uart_dm_idle(unsigned uart)
{
	struct uart *u = get(uart);
	return !u || ((readl(u->base + UART_DM_SR) & (SR_TXEMT | SR_RXRDY)) == SR_TXEMT && u->ring_head == u->ring_tail);
}

void uart_dm_hold_rx(unsigned uart, bool hold)
{
	struct uart *u = get(uart);
	if (!u)
		return;
	uint32_t mr1 = readl(u->base + UART_DM_MR1);
	if (hold) {
		writel(mr1 & ~MR1_RX_RDY_CTL, u->base + UART_DM_MR1);
		writel(CR_RESET_RFR, u->base + UART_DM_CR);
	} else {
		writel(CR_SET_RFR, u->base + UART_DM_CR);
		writel(mr1 | MR1_RX_RDY_CTL, u->base + UART_DM_MR1);
	}
}
