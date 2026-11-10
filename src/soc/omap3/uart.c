/* The OMAP's UARTs, a 16C750 kind with 64-byte FIFOs on a 48 MHz
 * clock. Reception is by interrupt into a ring; sending waits for room
 * in the FIFO. With flow control the UART itself holds RTS back when
 * its FIFO fills and stops sending while CTS is high. Some registers
 * only show with certain values in the line control register, which
 * is what the writes of 0xbf and 0x80 to it are for. */
#include "asteroid.h"

#define RHR	0x00
#define THR	0x00
#define DLL	0x00
#define IER	0x04
#define DLH	0x04
#define FCR	0x08
#define EFR	0x08
#define LCR	0x0c
#define MCR	0x10
#define LSR	0x14
#define MSR	0x18
#define TCR	0x18
#define TLR	0x1c
#define MDR1	0x20
#define SCR	0x40
#define SSR	0x44
#define SYSC	0x54
#define SYSS	0x58

#define IER_RX		(BIT(0) | BIT(2))
#define FCR_ENABLE	0x07		/* on, both emptied */
#define EFR_ENHANCED	BIT(4)
#define EFR_AUTO_RTS	BIT(6)
#define EFR_AUTO_CTS	BIT(7)
#define LCR_8N1		0x03
#define LCR_CONFIG_A	0x80
#define LCR_CONFIG_B	0xbf
#define MCR_RTS		BIT(1)
#define MCR_TCR_TLR	BIT(6)
#define LSR_RX		BIT(0)
#define LSR_OVERRUN	BIT(1)
#define LSR_TX_ROOM	BIT(5)
#define LSR_TX_EMPTY	BIT(6)
#define MSR_CTS		BIT(4)
#define SSR_TX_FULL	BIT(0)
#define MDR1_16X	0
#define MDR1_DISABLE	7
/* RTS goes up with 60 bytes waiting. */
#define TCR_HALT_60	0x0f

#define FCLK_HZ		48000000u
#define RING		4096u

struct uart {
	uint32_t base;
	bool up, flow, held;
	uint8_t ring[RING];
	volatile uint32_t head, tail;
	uint32_t rx, tx, overruns, dropped;
};

static struct uart uarts[2] = { { .base = UART1_BASE }, { .base = UART2_BASE } };

static struct uart *get(unsigned n)
{
	return n >= 1 && n <= 2 && uarts[n - 1].up ? &uarts[n - 1] : NULL;
}

static uint32_t rd(const struct uart *u, uint32_t reg) { return *(volatile uint16_t *)(uintptr_t)(u->base + reg); }
static void wr(const struct uart *u, uint32_t reg, uint32_t v) { *(volatile uint16_t *)(uintptr_t)(u->base + reg) = (uint16_t)v; }

static void rx_irq(struct uart *u)
{
	uint32_t lsr;
	while ((lsr = rd(u, LSR)) & LSR_RX) {
		uint8_t c = (uint8_t)rd(u, RHR);
		uint32_t next = (u->head + 1) % RING;
		if (lsr & LSR_OVERRUN)
			u->overruns++;
		if (next == u->tail) {
			u->dropped++;
			continue;
		}
		u->ring[u->head] = c;
		barrier();
		u->head = next;
		u->rx++;
	}
}

static void uart1_irq(void) { rx_irq(&uarts[0]); }
static void uart2_irq(void) { rx_irq(&uarts[1]); }

/* Flow control as set, or RTS held up by hand while the watch sleeps. */
static void lines(const struct uart *u)
{
	bool autoflow = u->flow && !u->held;
	wr(u, LCR, LCR_CONFIG_B);
	wr(u, EFR, autoflow ? EFR_AUTO_RTS | EFR_AUTO_CTS : 0);
	wr(u, LCR, LCR_8N1);
	wr(u, MCR, u->held ? 0 : MCR_RTS);
}

/* The UART corrupts data when a register is touched right after its
 * mode changes: a wait and empty FIFOs first. */
static void mode_set(const struct uart *u, uint32_t mode)
{
	wr(u, MDR1, mode);
	delay_us(2);
	wr(u, FCR, FCR_ENABLE);
	for (unsigned i = 0; i < 255 && (rd(u, LSR) & (LSR_TX_ROOM | LSR_RX)) != LSR_TX_ROOM; i++)
		delay_us(1);
}

static void baud_set(const struct uart *u, uint32_t baud)
{
	uint32_t div = (FCLK_HZ / 16 + baud / 2) / baud;
	mode_set(u, MDR1_DISABLE);
	wr(u, LCR, LCR_CONFIG_B);
	wr(u, EFR, EFR_ENHANCED);
	wr(u, LCR, 0);
	wr(u, IER, 0);
	wr(u, LCR, LCR_CONFIG_B);
	wr(u, DLL, div & 0xff);
	wr(u, DLH, div >> 8);
	wr(u, LCR, 0);
	wr(u, IER, IER_RX);
	lines(u);
	mode_set(u, MDR1_16X);
}

/* The pins are the board table's work: first_gpio and func are not
 * used here. */
int uart_init(unsigned n, unsigned first_gpio, unsigned func, uint32_t baud, bool flow)
{
	if (n < 1 || n > 2)
		return -1;
	struct uart *u = &uarts[n - 1];
	clock_enable(CM_CORE, n == 1 ? CORE_UART1 : CORE_UART2, true);
	irq_mask(n == 1 ? IRQ_UART1 : IRQ_UART2);
	wr(u, SYSC, BIT(1));
	for (unsigned i = 0; i < 100000 && !(rd(u, SYSS) & 1); i++)
		;
	/* No idle: its clocks are gated by hand. */
	wr(u, SYSC, 1u << 3);
	u->head = u->tail = 0;
	u->rx = u->tx = u->overruns = u->dropped = 0;
	u->flow = flow;
	u->held = false;
	mode_set(u, MDR1_DISABLE);
	wr(u, LCR, LCR_CONFIG_B);
	wr(u, EFR, EFR_ENHANCED);
	wr(u, LCR, LCR_CONFIG_A);
	wr(u, MCR, MCR_TCR_TLR);
	wr(u, TCR, TCR_HALT_60);
	wr(u, TLR, 0);
	wr(u, FCR, FCR_ENABLE);
	wr(u, SCR, 0);
	wr(u, MCR, 0);
	u->up = true;
	baud_set(u, baud);
	irq_attach(n == 1 ? IRQ_UART1 : IRQ_UART2, n == 1 ? uart1_irq : uart2_irq);
	return 0;
}

int uart_set_baud(unsigned n, uint32_t baud)
{
	struct uart *u = get(n);
	if (!u)
		return -1;
	uint32_t t0 = time_ms();
	while (!(rd(u, LSR) & LSR_TX_EMPTY))
		if (time_ms() - t0 > 100)
			return -1;
	uint32_t flags = irq_save();
	baud_set(u, baud);
	irq_restore(flags);
	return 0;
}

int uart_write(unsigned n, const uint8_t *p, unsigned len)
{
	struct uart *u = get(n);
	if (!u)
		return -1;
	uint32_t t0 = time_ms();
	for (unsigned i = 0; i < len;) {
		if (rd(u, SSR) & SSR_TX_FULL) {
			if (time_ms() - t0 > 1000)
				return -1;
			continue;
		}
		wr(u, THR, p[i++]);
		t0 = time_ms();
	}
	u->tx += len;
	return 0;
}

unsigned uart_read(unsigned n, uint8_t *p, unsigned len)
{
	struct uart *u = get(n);
	unsigned k = 0;
	if (!u)
		return 0;
	while (k < len && u->tail != u->head) {
		p[k++] = u->ring[u->tail];
		barrier();
		u->tail = (u->tail + 1) % RING;
	}
	return k;
}

/* Whether the other side lets this one send. */
bool uart_cts(unsigned n)
{
	struct uart *u = get(n);
	return u && (rd(u, MSR) & MSR_CTS);
}

void uart_stop(unsigned n)
{
	struct uart *u = get(n);
	if (!u)
		return;
	irq_detach(n == 1 ? IRQ_UART1 : IRQ_UART2);
	wr(u, IER, 0);
	wr(u, MDR1, MDR1_DISABLE);
	u->up = false;
	clock_enable(CM_CORE, n == 1 ? CORE_UART1 : CORE_UART2, false);
}

bool uart_idle(unsigned n)
{
	struct uart *u = get(n);
	return !u || ((rd(u, LSR) & (LSR_TX_EMPTY | LSR_RX)) == LSR_TX_EMPTY && u->head == u->tail);
}

void uart_hold_rx(unsigned n, bool hold)
{
	struct uart *u = get(n);
	if (!u || u->held == hold)
		return;
	u->held = hold;
	lines(u);
}

void uart_stats(unsigned n, uint32_t *rx, uint32_t *tx, uint32_t *errors)
{
	struct uart *u = get(n);
	*rx = u ? u->rx : 0;
	*tx = u ? u->tx : 0;
	*errors = u ? u->dropped + u->overruns : 0;
}
