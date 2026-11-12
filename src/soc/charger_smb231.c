/* SMB231 linear charger on I2C, with the cell's numbers from the sensor
 * hub's fuel gauge. The charger takes its settings from registers: 4.4 V,
 * 300 mA, 20 mA termination, recharge 80 mV below, input limited to
 * 500 mA, charging between -5 and 55 C and slower outside 5 to 45 C. It
 * can reload its own defaults when the cable goes, so they are written
 * again on every cable change, and until they are taken. Its STAT line
 * falls on a change and stays low until its interrupt registers are
 * read. USB is there while the input is neither under- nor over-voltage;
 * with a cable it is looked at every second. */
#include "asteroid.h"

#define BUS		(board->charger.i2c.bus)
#define ADDR		(board->charger.i2c.addr)
#define CFG0		0x00
#define CFG2		0x02
#define CFG3		0x03
#define CFG4		0x04
#define CFG5		0x05
#define CFG6		0x06
#define CFG7		0x07
#define CFG8		0x08
#define IRQ_CFG		0x09
#define I2C_COMM_CFG	0x0a
#define CMD0		0x30
#define CMD1		0x31
#define IRQ_A		0x38
#define IRQ_C		0x3a
#define STATUS_A	0x3c
#define STATUS_B	0x3d
#define CMD0_VOLATILE	BIT(7)
#define CMD0_SUSPEND	BIT(2)
#define CMD0_CHARGE	BIT(1)
#define CMD0_STAT_OFF	BIT(0)
#define IRQ_C_DONE	BIT(0)
#define STATUS_A_OV	BIT(6)
#define STATUS_A_UV	BIT(4)
#define POLL_MS		60000
#define CABLE_POLL_MS	1000

static bool ready, configured, cable_now, irq_seen;
static uint32_t poll_t, errors;

static int rd(uint8_t reg, uint8_t *v) { return i2c_xfer(BUS, ADDR, &reg, 1, v, 1); }

static int wr(uint8_t reg, uint8_t v)
{
	uint8_t b[2] = { reg, v };
	return i2c_xfer(BUS, ADDR, b, 2, NULL, 0);
}

static int update(uint8_t reg, uint8_t mask, uint8_t v)
{
	uint8_t old;
	int r = rd(reg, &old);
	return r ? r : wr(reg, (uint8_t)((old & ~mask) | v));
}

static bool vbus(void)
{
	uint8_t a;
	return ready && !rd(STATUS_A, &a) && !(a & (STATUS_A_OV | STATUS_A_UV));
}

static bool configure(void)
{
	uint8_t comm;
	int r = rd(I2C_COMM_CFG, &comm);
	if (r || !(comm & 1)) {
		log_printf("charger: registers not writable (%d, %02x)\n", r, comm);
		return false;
	}
	r = update(CMD0, CMD0_VOLATILE | CMD0_CHARGE, CMD0_VOLATILE | CMD0_CHARGE);
	if (!r)
		r = update(CFG0, 0x1f, 2 << 2 | 0);
	if (!r)
		r = update(CFG2, BIT(6) | 7, BIT(6) | 2);
	if (!r)
		r = update(CFG3, 0xff, 1 << 5 | (4400 - 3480) / 40);
	if (!r)
		r = update(CFG4, BIT(5) | 3 << 3 | 3, 2);
	if (!r)
		r = update(CFG5, 0xff, 3 << 4 | 3 << 2 | 1);
	if (!r)
		r = update(CFG6, 3 << 6 | 3 << 4, 2 << 4);
	if (!r)
		r = update(CFG7, 0xe8, 0x08);
	if (!r)
		r = wr(CFG8, 3 << 6 | 1 << 4 | 2 << 2 | 1);
	if (!r)
		r = wr(IRQ_CFG, 0xbf);
	if (!r)
		r = update(CMD1, 3, 2);
	if (!r)
		r = update(CMD0, CMD0_SUSPEND | CMD0_STAT_OFF, CMD0_STAT_OFF);
	if (r) {
		errors++;
		log_printf("charger: setup failed (%d)\n", r);
	}
	return !r;
}

static void stat_edge(unsigned n)
{
	gpio_irq_ack(n);
	irq_seen = true;
}

static void smb231_init(void)
{
	gpio_set(board->charger.suspend, 1);
	gpio_config(board->charger.suspend, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_config(board->charger.irq, 0, GPIO_INPUT, GPIO_PULL_UP, 2);
	const struct board_i2c *b = &board->charger.i2c;
	if (i2c_init(b->bus, b->sda, b->scl, b->func, 100000))
		return;
	uint8_t a;
	if (rd(STATUS_A, &a)) {
		log_printf("charger: no answer\n");
		return;
	}
	ready = true;
	configured = configure();
	cable_now = vbus();
	log_printf("charger: smb231 %s, usb %u\n", configured ? "set up" : "not set up", cable_now);
	irq_seen = true;
}

static uint32_t poll_ms(void) { return cable_now || !configured ? CABLE_POLL_MS : POLL_MS; }

static void smb231_poll(void)
{
	uint32_t now = time_ms();
	if (!ready || (!irq_seen && now - poll_t < poll_ms()))
		return;
	irq_seen = false;
	poll_t = now;
	uint8_t irqs[4];
	uint8_t reg = IRQ_A;
	i2c_xfer(BUS, ADDR, &reg, 1, irqs, sizeof irqs);
	bool c = vbus();
	if (c != cable_now || !configured)
		configured = configure();
	cable_now = c;
	if (board->hub && board->hub->poll_gauge)
		board->hub->poll_gauge();
}

static uint32_t smb231_next_ms(void)
{
	uint32_t gone = time_ms() - poll_t;
	return irq_seen ? 1 : gone >= poll_ms() ? 1 : poll_ms() - gone;
}

static bool gauge(struct hub_gauge *g)
{
	return board->hub && board->hub->gauge && board->hub->gauge(g);
}

static uint32_t smb231_percent(void)
{
	struct hub_gauge g;
	return gauge(&g) ? g.percent : 50;
}

static bool charge_done(void)
{
	uint8_t c;
	return ready && !rd(IRQ_C, &c) && (c & IRQ_C_DONE);
}

static bool smb231_charging(void)
{
	uint8_t b;
	return ready && !rd(STATUS_B, &b) && (b & 6);
}

static bool smb231_cable(void) { return cable_now; }

static void smb231_cable_irq_init(void)
{
	gpio_irq_config(board->charger.irq, GPIO_IRQ_EDGE_FALLING, stat_edge);
	gpio_irq_enable(board->charger.irq, true);
	if (board->charger.mpm)
		wake_pin(board->charger.mpm, true, false);
}

static bool smb231_cable_irq(void)
{
	bool before = cable_now;
	if (irq_seen)
		smb231_poll();
	return cable_now != before;
}

static uint32_t smb231_mv(void)
{
	struct hub_gauge g;
	return gauge(&g) ? g.mv : 0;
}

static int32_t smb231_ua(void)
{
	struct hub_gauge g;
	return gauge(&g) ? g.ua : 0;
}

static const char *smb231_status(void)
{
	static char s[96];
	struct hub_gauge g = { 0 };
	bool have = gauge(&g);
	snprintf(s, sizeof s, "usb %u charging %u done %u, %s %lu%% %lu mV %d.%d C, %lu errors", cable_now, smb231_charging(),
		 charge_done(), have ? "gauge" : "no gauge", have ? g.percent : 50, g.mv, g.decidegrees / 10,
		 (g.decidegrees < 0 ? -g.decidegrees : g.decidegrees) % 10, errors);
	return s;
}

const struct battery_driver battery_smb231 = {
	.init = smb231_init,
	.poll = smb231_poll,
	.next_ms = smb231_next_ms,
	.percent = smb231_percent,
	.charging = smb231_charging,
	.cable = smb231_cable,
	.usb = smb231_cable,
	.cable_irq_init = smb231_cable_irq_init,
	.cable_irq = smb231_cable_irq,
	.mv = smb231_mv,
	.ua = smb231_ua,
	.avg_ua = smb231_ua,
	.status = smb231_status,
};
