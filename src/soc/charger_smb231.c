/* SMB231 linear charger on I2C, with the cell's numbers from the board's
 * fuel gauge or the sensor hub's, and its settings from the board. It
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
#define CFG3		0x03
#define FLOAT_MASK	0x1f
#define FLOAT_CODE(mv)	(((mv) - 3480) / 40)
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
#define IRQS_AWAKE	(board->charger.irqs_awake)
#define IRQS_ASLEEP	(board->charger.irqs_asleep)
#define POLL_MS		60000
#define CABLE_POLL_MS	1000
#define GAUGE_ASK_MS	60000

#define EMPTY_POLLS	2
#define CELL_MAH	(board->charger.cell_mah)
/* The PM8916 linear charger's USB input suspend. */
#define LBC_USB_SUSP	0x1347
#define CHARGER_TASK_MS	500

static bool ready, configured, cable_now, cable_told, irq_seen, have_seen, fresh, stale, keeps_settings, asleep;
static uint32_t poll_t, gauge_ask_t, errors, empty_polls, quiet_polls;
static bool gauge_asked, trimmed;
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

/* 1 with USB, 0 without, -1 when the charger does not answer. */
static int vbus(void)
{
	uint8_t a;
	if (!ready || rd(STATUS_A, &a))
		return -1;
	return !(a & (STATUS_A_OV | STATUS_A_UV));
}

/* A setup register that does not read back as written. */
static int check(const struct board_reg *s)
{
	uint8_t v;
	int r = rd(s->reg, &v);
	if (!r && (v & (s->mask ? s->mask : 0xff)) != s->value) {
		log_printf("charger: reg %02x reads %02x, wrote %02x\n", s->reg, v, s->value);
		r = -5;
	}
	return r;
}

/* Charging stays off until every limit is in place and reads back, and
 * while the PMIC's own charger could still be running beside it. */
static bool configure(void)
{
	uint8_t comm;
	int r = rd(I2C_COMM_CFG, &comm);
	if (r || !(comm & 1)) {
		log_printf("charger: registers not writable (%d, %02x)\n", r, comm);
		return false;
	}
	r = update(CMD0, CMD0_VOLATILE | CMD0_CHARGE, CMD0_VOLATILE);
	for (unsigned i = 0; !r && i < board->charger.setup_n; i++) {
		const struct board_reg *s = &board->charger.setup[i];
		r = s->mask ? update(s->reg, s->mask, s->value) : wr(s->reg, s->value);
	}
	for (unsigned i = 0; !r && i < board->charger.setup_n; i++)
		r = check(&board->charger.setup[i]);
	if (!r && trimmed)
		r = update(CFG3, FLOAT_MASK, FLOAT_CODE(board->charger.ov_float_mv));
	if (!r)
		r = wr(IRQ_CFG, asleep ? IRQS_ASLEEP : IRQS_AWAKE);
	if (!r)
		r = update(CMD1, 3, board->charger.cmd1);
	if (!r)
		r = update(CMD0, CMD0_SUSPEND | CMD0_STAT_OFF, CMD0_STAT_OFF);
	if (!r && board->charger.pmic_charger_off) {
		pmic_write(LBC_USB_SUSP, 1);
		if (!(pmic_read(LBC_USB_SUSP) & 1))
			r = -6;
	}
	if (!r)
		r = update(CMD0, CMD0_CHARGE, CMD0_CHARGE);
	if (r) {
		errors++;
		log_printf("charger: setup failed (%d), charging off\n", r);
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
	if (board->charger.hold_low) {
		gpio_set(board->charger.hold_low, 0);
		gpio_config(board->charger.hold_low, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	}
	if (board->charger.pmic_charger_off)
		pmic_write(LBC_USB_SUSP, 1);
	const struct board_i2c *b = &board->charger.i2c;
	if (i2c_init(b->bus, b->sda, b->scl, b->func, 100000))
		return;
	if (board->fuel_gauge && !board->fuel_gauge->init())
		log_printf("charger: no fuel gauge\n");
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
	if (!rd(NV_CFG, &nv)) {
		keeps_settings = nv & NV_UNPLUG_KEEP;
		log_printf("charger: nv cfg %02x, %s\n", nv,
			   nv & NV_UNPLUG_KEEP ? "settings kept on unplug, irq only" : "defaults on unplug, needs polling");
	}
	configured = configure();
	cable_now = cable_told = vbus() == 1;
	log_printf("charger: smb231 %s, usb %u\n", configured ? "set up" : "not set up", cable_now);
	irq_seen = true;
}

static uint32_t smb231_percent(void);
static uint32_t smb231_mv(void);

/* Nothing else turns the watch off before the cell runs flat (catfish's
 * hub stops at 3072 mV), so without a cable it powers off, through
 * Timepiece where the hub has one like the vendor's at shutdown: at 0 %,
 * or like swift's vendor after five readings at or below empty_mv. Each
 * reading counts once. */
#define EMPTY_MV_POLLS	5

static void check_empty(uint32_t percent, uint32_t mv, bool pmic)
{
	bool by_mv = board->charger.empty_mv;
	bool low = by_mv ? mv && mv <= board->charger.empty_mv : percent == 0;
	if (cable_now || !low || pmic != empty_from_pmic) {
		empty_polls = 0;
		empty_from_pmic = pmic;
		if (cable_now || !low)
			return;
	}
	if (++empty_polls < (by_mv ? EMPTY_MV_POLLS : EMPTY_POLLS))
		return;
	log_printf("charger: battery empty (%lu %%, %lu mV), powering off\n", percent, mv);
	if (hub_has_timepiece())
		power_timepiece();
	power_off_arm();
	reboot();
}

/* As the vendor's gauge driver does: a cell that reaches ov_mv on the
 * cable is charged to the lower float voltage for the rest of it. */
static void overvoltage(uint32_t mv)
{
	if (!board->charger.ov_mv || !cable_now || trimmed || mv < board->charger.ov_mv)
		return;
	trimmed = true;
	int r = update(CFG3, FLOAT_MASK, FLOAT_CODE(board->charger.ov_float_mv));
	if (r)
		configured = false;
	log_printf("charger: %lu mV, float voltage %u mV (%d)\n", mv, board->charger.ov_float_mv, r);
}

static uint32_t poll_ms(void) { return (cable_now && !keeps_settings) || !configured ? CABLE_POLL_MS : POLL_MS; }

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
		int v = vbus();
		if (v < 0)
			errors++;
		bool c = v < 0 ? cable_now : v, changed = c != cable_now;
		if (changed && !c)
			trimmed = false;
		if (changed || !configured)
			configured = configure();
		cable_now = c;
		if (changed || !gauge_asked || now - gauge_ask_t >= GAUGE_ASK_MS) {
			gauge_asked = true;
			gauge_ask_t = now;
			if (board->fuel_gauge)
				board->fuel_gauge->poll(changed);
			else if (board->hub && board->hub->poll_gauge)
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
		overvoltage(seen.mv);
		check_empty(seen.percent, seen.mv, false);
	} else if (due && stale) {
		uint32_t mv = pmic_battery_mv();
		if (mv)
			check_empty(percent_from_mv(mv), mv, true);
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
	if (board->fuel_gauge)
		return board->fuel_gauge->read(g);
	return board->hub && board->hub->gauge && board->hub->gauge(g);
}

static bool reading_new(void)
{
	struct hub_gauge g;
	return gauge(&g) && (!have_seen || g.t_ms != seen.t_ms);
}

/* Until the gauge has reported, a few seconds after boot, the charge
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

static void smb231_asleep(bool a)
{
	asleep = a;
	if (board->fuel_gauge && board->fuel_gauge->asleep)
		board->fuel_gauge->asleep(a);
	if (ready && configured && wr(IRQ_CFG, a ? IRQS_ASLEEP : IRQS_AWAKE))
		errors++;
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
	.asleep = smb231_asleep,
	.mv = smb231_mv,
	.ua = smb231_ua,
	.ua_age_ms = smb231_ua_age_ms,
	.status = smb231_status,
};
