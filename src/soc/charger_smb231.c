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
#include "power.h"
#include "sensors/hub.h"
#include "soc/charge_policy.h"

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
#define NV_CFG		0x14
#define CMD0		0x30
#define CMD1		0x31
#define IRQ_A		0x38
#define IRQ_C		0x3a
#define STATUS_A	0x3c
#define STATUS_B	0x3d
#define NV_UNPLUG_KEEP	BIT(2)
#define CMD0_VOLATILE	BIT(7)
#define CMD0_SUSPEND	BIT(2)
#define CMD0_CHARGE	BIT(1)
#define CMD0_STAT_OFF	BIT(0)
#define IRQ_C_DONE	BIT(0)
#define STATUS_A_OV	BIT(6)
#define STATUS_A_UV	BIT(4)
#define POLL_MS		60000
#define CABLE_POLL_MS	1000
#define GAUGE_ASK_MS	60000

#define EMPTY_POLLS	2
#define CELL_MAH	415
#define CHARGER_TASK_MS	500

static bool ready, configured, cable_now, cable_told, irq_seen, have_seen, fresh, stale;
static uint32_t poll_t, gauge_ask_t, errors, empty_polls, quiet_polls;
static bool gauge_asked;
static bool empty_from_pmic;
/* The last gauge reading taken in. */
static struct hub_gauge seen;

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
	/* As the vendor driver reads it: without the bit the chip reloads
	 * its defaults when the cable goes, and its STAT interrupt cannot be
	 * counted on while plugged in. */
	uint8_t nv;
	if (!rd(NV_CFG, &nv))
		log_printf("charger: nv cfg %02x, %s\n", nv,
			   nv & NV_UNPLUG_KEEP ? "settings kept on unplug, irq only" : "defaults on unplug, needs polling");
	configured = configure();
	cable_now = cable_told = vbus();
	log_printf("charger: smb231 %s, usb %u\n", configured ? "set up" : "not set up", cable_now);
	irq_seen = true;
}

static uint32_t smb231_percent(void);
static uint32_t smb231_mv(void);

/* The hub's microcontroller stops at 3072 mV, and nothing else turns the
 * watch off before the cell runs flat, so at 0 % without a cable it goes
 * into Timepiece like the vendor's at shutdown, which powers it off.
 * Each reading counts once. */
static void check_empty(uint32_t percent, bool pmic)
{
	if (cable_now || percent > 0 || pmic != empty_from_pmic) {
		empty_polls = 0;
		empty_from_pmic = pmic;
		if (cable_now || percent > 0)
			return;
	}
	if (++empty_polls < EMPTY_POLLS)
		return;
	log_printf("charger: battery empty, powering off\n");
	if (hub_has_timepiece())
		power_timepiece();
	power_off_arm();
	reboot();
}

static uint32_t poll_ms(void) { return cable_now || !configured ? CABLE_POLL_MS : POLL_MS; }

static bool gauge(struct hub_gauge *g);
static bool reading_new(void);
static uint32_t percent_from_mv(uint32_t mv);

/* The gauge is asked at most every GAUGE_ASK_MS, and on a cable change.
 * A hub that sent nothing over two of those is not believed: the PMIC's
 * voltage stands in. */
static void smb231_poll(void)
{
	uint32_t now = time_ms();
	if (!ready)
		return;
	bool due = irq_seen || now - poll_t >= poll_ms();
	if (due) {
		irq_seen = false;
		poll_t = now;
		uint8_t irqs[4];
		uint8_t reg = IRQ_A;
		i2c_xfer(BUS, ADDR, &reg, 1, irqs, sizeof irqs);
		bool c = vbus(), changed = c != cable_now;
		if (changed || !configured)
			configured = configure();
		cable_now = c;
		if (changed || !gauge_asked || now - gauge_ask_t >= GAUGE_ASK_MS) {
			gauge_asked = true;
			gauge_ask_t = now;
			if (board->hub && board->hub->poll_gauge)
				board->hub->poll_gauge(changed);
			quiet_polls = fresh || reading_new() ? 0 : quiet_polls + 1;
			stale = quiet_polls >= 2;
			fresh = false;
		}
	}
	/* After the cable is known, so a plug-in is not taken for empty. */
	if (reading_new()) {
		gauge(&seen);
		have_seen = fresh = true;
		check_empty(seen.percent, false);
	} else if (due && stale) {
		uint32_t mv = pmic_battery_mv();
		if (mv)
			check_empty(percent_from_mv(mv), true);
	}
}

/* Asleep without a cable, the next look is when the cell could have
 * run empty since the newest reading, or soon to confirm an empty one.
 * A new reading waits for the next wake to be checked. */
static uint32_t smb231_next_ms(void)
{
	if (!ready)
		return UINT32_MAX;
	uint32_t now = time_ms(), gone = now - poll_t;
	struct hub_gauge g;
	if (irq_seen)
		return 1;
	uint32_t next;
	if (power_awake() || cable_now || !configured || empty_polls || stale || !gauge(&g)) {
		next = gone >= poll_ms() ? 1 : poll_ms() - gone;
	} else {
		uint32_t wait = charge_empty_wait_ms(g.percent, g.ua, CELL_MAH), left = ms_left(g.t_ms, wait, now);
		next = gone < POLL_MS && left < POLL_MS - gone ? POLL_MS - gone : left;
	}
	/* Asleep the poll comes from the charger task, run at most every
	 * CHARGER_TASK_MS: sooner would only wake for nothing. */
	return !power_awake() && next < CHARGER_TASK_MS ? CHARGER_TASK_MS : next;
}

static bool gauge(struct hub_gauge *g)
{
	return board->hub && board->hub->gauge && board->hub->gauge(g);
}

static bool reading_new(void)
{
	struct hub_gauge g;
	return gauge(&g) && (!have_seen || g.t_ms != seen.t_ms);
}

/* Until the hub's gauge has reported, a few seconds after boot, the charge
 * comes from the cell voltage the PMIC measures: a rough curve for a
 * 4.4 V cell, reading high while it charges. */
static const struct {
	uint16_t mv;
	uint8_t percent;
} curve[] = {
	{ 3400, 0 }, { 3600, 5 }, { 3700, 12 }, { 3780, 25 }, { 3850, 40 },
	{ 3950, 55 }, { 4100, 75 }, { 4250, 90 }, { 4380, 100 },
};

static uint32_t percent_from_mv(uint32_t mv)
{
	if (mv <= curve[0].mv)
		return 0;
	for (unsigned i = 1; i < ARRAY_SIZE(curve); i++)
		if (mv < curve[i].mv)
			return curve[i - 1].percent + (mv - curve[i - 1].mv) * (uint32_t)(curve[i].percent - curve[i - 1].percent) /
							      (uint32_t)(curve[i].mv - curve[i - 1].mv);
	return 100;
}

static uint32_t smb231_percent(void)
{
	struct hub_gauge g;
	return gauge(&g) ? g.percent : percent_from_mv(pmic_battery_mv());
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

/* Also a change the charger task's poll saw first. */
static bool smb231_cable_irq(void)
{
	if (irq_seen)
		smb231_poll();
	bool changed = cable_now != cable_told;
	cable_told = cable_now;
	return changed;
}

static uint32_t smb231_mv(void)
{
	struct hub_gauge g;
	return gauge(&g) ? g.mv : pmic_battery_mv();
}

static int32_t smb231_ua(void)
{
	struct hub_gauge g;
	return gauge(&g) ? g.ua : 0;
}

static uint32_t smb231_ua_age_ms(void)
{
	struct hub_gauge g;
	return gauge(&g) ? time_ms() - g.t_ms : UINT32_MAX;
}

static const char *smb231_status(void)
{
	static char s[96];
	struct hub_gauge g = { 0 };
	bool have = gauge(&g);
	snprintf(s, sizeof s, "usb %u charging %u done %u, %s %lu%% %lu mV %d.%d C, %lu errors", cable_now, smb231_charging(),
		 charge_done(), have ? "gauge" : "no gauge", have ? g.percent : smb231_percent(), have ? g.mv : smb231_mv(),
		 g.decidegrees / 10,
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
	.ua_age_ms = smb231_ua_age_ms,
	.status = smb231_status,
};
