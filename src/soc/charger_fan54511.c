/* Fairchild FAN54511 switching charger on I2C, with the cell's numbers
 * from the board's fuel gauge, its fixed settings from the board and the
 * rest as the vendor's driver runs it. Its interrupt line goes low on a
 * change and stays low until the interrupt registers are read.
 *
 * The vendor ends the charge itself: the chip's termination current is
 * set to its highest, so that its end-of-charge interrupt marks the
 * start of constant voltage, and with the top-off timer on the chip
 * charges on from there. Once the gauge has seen no more than ITERM_MA
 * going in four times 10 s apart, charging is switched off as full, and
 * on again when the percentage falls under 100. Above STEP_MV the USB
 * input is cut to STEP_IBUS_MA until the cell is STEP_BACK_MV under it.
 *
 * The cell's temperature comes from the PMIC's thermistor input, as the
 * vendor's thermal table has it: 4.1 V from 45 C, where reaching
 * constant voltage ends the charge until the cell cools under 43 C or
 * has lost 1 %, no charging from 55 C or below -10 C, back under 43 C
 * and from -5 C. Without a reading, charging is held off. */
#include "asteroid.h"
#include "power.h"
#include "soc/charge_policy.h"

#define BUS		(board->charger.i2c.bus)
#define ADDR		(board->charger.i2c.addr)
#define STAT0		0x00
#define STAT1		0x01
#define INT0		0x04
#define CON2		0x0e
#define CON3		0x0f
#define VFLOAT		0x11
#define IBUS		0x14
#define MNT0		0x20
#define IC_INFO		0x2d
#define STAT0_STAT	BIT(3)
#define STAT0_VBUSPWR	BIT(5)
#define STAT1_TOCHG	BIT(3)
#define INT0_CHGEND	BIT(3)
#define CON2_TOEN	BIT(2)
#define CON2_TE		BIT(3)
#define CON3_CE_OFF	BIT(0)
#define MNT0_CV		BIT(0)
#define IC_INFO_PN(v)	(((v) >> 3) & 7)

#define FLOAT_CODE(mv)	(((mv) - 3300) / 10)
#define IBUS_CODE(ma)	(((ma) - 100) / 25)
#define FLOAT_MV	4400
#define WARM_FLOAT_MV	4100
#define RECHARGE_MV	(FLOAT_MV - 100)
#define IBUS_MA		400
#define STEP_IBUS_MA	150
#define STEP_MV		4100
#define STEP_BACK_MV	(STEP_MV - 200)
#define ITERM_MA	25
#define EOC_COUNT	3
#define TE_TOGGLE_MS	300

#define WARM		450
#define HOT_STOP	550
#define HOT_RESUME	430
#define COLD_STOP	(-100)
#define COLD_RESUME	(-50)

#define POLL_MS		60000
#define CABLE_POLL_MS	10000
#define CHARGER_TASK_MS	500
#define EMPTY_POLLS	2
/* The PM8916 linear charger's USB input suspend. */
#define LBC_USB_SUSP	0x1347

static bool ready, configured, cable_now, cable_told, cv, eoc, full, warm, too_hot, too_cold, no_temp, stepped;
static volatile bool irq_seen;
static unsigned eoc_count, empty_polls;
static uint32_t poll_t, errors, eoc_percent;
/* What is in the chip, so that only changes are written. */
static int set_float = -1, set_ibus = -1, set_off = -1;

static int rd(uint8_t reg, uint8_t *v)
{
	int r = i2c_xfer(BUS, ADDR, &reg, 1, v, 1);
	if (r)
		errors++;
	return r;
}

static int wr(uint8_t reg, uint8_t v)
{
	uint8_t b[2] = { reg, v };
	int r = i2c_xfer(BUS, ADDR, b, 2, NULL, 0);
	if (r)
		errors++;
	return r;
}

static int update(uint8_t reg, uint8_t mask, uint8_t v)
{
	uint8_t old;
	int r = rd(reg, &old);
	return r ? r : wr(reg, (uint8_t)((old & ~mask) | v));
}

static bool gauge(struct hub_gauge *g) { return board->fuel_gauge && board->fuel_gauge->read(g); }

/* Charging stays off until every fixed setting reads back. */
static bool configure(void)
{
	int r = update(CON3, CON3_CE_OFF, CON3_CE_OFF);
	for (unsigned i = 0; !r && i < board->charger.setup_n; i++) {
		const struct board_reg *s = &board->charger.setup[i];
		r = s->mask ? update(s->reg, s->mask, s->value) : wr(s->reg, s->value);
	}
	for (unsigned i = 0; !r && i < board->charger.setup_n; i++) {
		const struct board_reg *s = &board->charger.setup[i];
		uint8_t v;
		r = rd(s->reg, &v);
		if (!r && (v & (s->mask ? s->mask : 0xff)) != s->value) {
			log_printf("charger: reg %02x reads %02x, wrote %02x\n", s->reg, v, s->value);
			r = -5;
		}
	}
	if (!r && board->charger.pmic_charger_off) {
		pmic_write(LBC_USB_SUSP, 1);
		if (!(pmic_read(LBC_USB_SUSP) & 1))
			r = -6;
	}
	set_float = set_ibus = -1;
	set_off = 1;
	if (r)
		log_printf("charger: setup failed (%d), charging off\n", r);
	return !r;
}

/* The limits for now, each written when it changes. */
static void apply(void)
{
	int fl = warm ? WARM_FLOAT_MV : FLOAT_MV, ib = stepped ? STEP_IBUS_MA : IBUS_MA;
	int off = !configured || !cable_now || eoc || too_hot || too_cold || no_temp;
	int r = 0;
	if (fl != set_float) {
		/* Above the lower float voltage the top-off would charge on to
		 * the full one's end, so it is off while warm. */
		r = update(CON2, CON2_TOEN, warm ? 0 : CON2_TOEN);
		if (!r)
			r = wr(VFLOAT, (uint8_t)FLOAT_CODE(fl));
		set_float = r ? -1 : fl;
	}
	if (!r && ib != set_ibus) {
		r = wr(IBUS, (uint8_t)IBUS_CODE(ib));
		set_ibus = r ? -1 : ib;
	}
	if (off != set_off) {
		if (update(CON3, CON3_CE_OFF, off ? CON3_CE_OFF : 0))
			set_off = -1;
		else
			set_off = off;
	}
}

static void temperature(void)
{
	int t;
	if (!pmic_battery_temp(&t)) {
		if (!no_temp)
			log_printf("charger: no temperature, charging held off\n");
		no_temp = true;
		return;
	}
	no_temp = false;
	if (board->fuel_gauge && board->fuel_gauge->host_temp)
		board->fuel_gauge->host_temp(t);
	bool hot = too_hot ? t >= HOT_RESUME : t >= HOT_STOP;
	bool cold = too_cold ? t < COLD_RESUME : t <= COLD_STOP;
	bool w = warm ? t >= HOT_RESUME : t >= WARM;
	if (hot != too_hot || cold != too_cold || w != warm)
		log_printf("charger: %d.%d C, charging %s\n", t / 10, (t < 0 ? -t : t) % 10,
			   hot || cold ? "held off" : w ? "at 4.1 V" : "allowed");
	too_hot = hot;
	too_cold = cold;
	warm = w;
	if (t >= CHARGE_POWER_OFF_TEMP) {
		log_printf("charger: %d.%d C, powering off\n", t / 10, t % 10);
		power_off_arm();
		reboot();
	}
}

static void end_charge(bool is_full, uint32_t percent)
{
	eoc = true;
	full = is_full;
	cv = false;
	eoc_count = 0;
	eoc_percent = percent;
	log_printf("charger: %s at %lu%%\n", is_full ? "full" : "charged to 4.1 V", percent);
}

/* The vendor's software end of charge, from the start of constant voltage. */
static void eoc_check(const struct hub_gauge *g)
{
	int32_t charge_ma = -g->ua / 1000;
	if (g->mv <= RECHARGE_MV) {
		/* The termination comparator rearmed for the next start. */
		update(CON2, CON2_TE, 0);
		delay_ms(TE_TOGGLE_MS);
		update(CON2, CON2_TE, CON2_TE);
		cv = false;
		eoc_count = 0;
	} else if (charge_ma > ITERM_MA || g->ua > 0) {
		eoc_count = 0;
	} else if (eoc_count >= EOC_COUNT) {
		end_charge(true, g->percent);
	} else {
		eoc_count++;
	}
}

static void recharge_check(const struct hub_gauge *g)
{
	if (!eoc || (full ? g->percent > 99 : g->percent + 1 > eoc_percent && warm))
		return;
	log_printf("charger: charging again at %lu%%\n", g->percent);
	eoc = full = false;
}

static void check_empty(uint32_t percent)
{
	if (cable_now || percent) {
		empty_polls = 0;
		return;
	}
	if (++empty_polls < EMPTY_POLLS)
		return;
	log_printf("charger: battery empty, powering off\n");
	power_off_arm();
	reboot();
}

static void fan_poll(void)
{
	uint32_t now = time_ms();
	if (!ready)
		return;
	bool due = irq_seen || now - poll_t >= (cable_now || !configured ? CABLE_POLL_MS : POLL_MS);
	if (!due)
		return;
	irq_seen = false;
	poll_t = now;
	uint8_t ints[3] = { 0 }, stat = 0, mnt = 0;
	uint8_t reg = INT0;
	if (i2c_xfer(BUS, ADDR, &reg, 1, ints, sizeof ints))
		errors++;
	bool c = cable_now;
	if (!rd(STAT0, &stat))
		c = stat & STAT0_VBUSPWR;
	bool changed = c != cable_now;
	cable_now = c;
	if (changed || !configured)
		configured = configure();
	if (changed && !c) {
		cv = eoc = full = stepped = false;
		eoc_count = 0;
	}
	temperature();
	if (board->fuel_gauge)
		board->fuel_gauge->poll(changed);
	struct hub_gauge g;
	bool have = gauge(&g);
	if (cable_now && have) {
		stepped = stepped ? g.mv >= STEP_BACK_MV : g.mv >= STEP_MV;
		bool in_cv = !rd(MNT0, &mnt) && (mnt & MNT0_CV);
		if (!eoc && set_off == 0 && ((ints[0] & INT0_CHGEND) || in_cv)) {
			if (warm)
				end_charge(false, g.percent);
			else
				cv = true;
		}
		if (cv && !eoc && set_off == 0)
			eoc_check(&g);
		recharge_check(&g);
	}
	apply();
	if (have)
		check_empty(g.percent);
}

static void irq_edge(unsigned n)
{
	gpio_irq_ack(n);
	irq_seen = true;
}

static void fan_init(void)
{
	if (board->charger.hold_low) {
		gpio_set(board->charger.hold_low, 0);
		gpio_config(board->charger.hold_low, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	}
	gpio_config(board->charger.irq, 0, GPIO_INPUT, GPIO_PULL_UP, 2);
	if (board->charger.pmic_charger_off)
		pmic_write(LBC_USB_SUSP, 1);
	const struct board_i2c *b = &board->charger.i2c;
	if (i2c_init(b->bus, b->sda, b->scl, b->func, 100000))
		return;
	if (board->fuel_gauge && !board->fuel_gauge->init())
		log_printf("charger: no fuel gauge\n");
	uint8_t info;
	if (rd(IC_INFO, &info)) {
		log_printf("charger: no answer\n");
		return;
	}
	ready = true;
	irq_seen = true;
	poll_t = time_ms();
	fan_poll();
	log_printf("charger: fan5451x part %u rev %u, %s, usb %u\n", IC_INFO_PN(info), info & 7,
		   configured ? "set up" : "not set up", cable_now);
	cable_told = cable_now;
}

/* Asleep without a cable, the next look is when the cell could have
 * run empty since the newest reading. */
static uint32_t fan_next_ms(void)
{
	if (!ready)
		return UINT32_MAX;
	if (irq_seen)
		return 1;
	uint32_t now = time_ms(), gone = now - poll_t, every = cable_now || !configured ? CABLE_POLL_MS : POLL_MS;
	struct hub_gauge g;
	uint32_t next;
	if (power_awake() || cable_now || !configured || empty_polls || !gauge(&g))
		next = gone >= every ? 1 : every - gone;
	else
		next = ms_left(g.t_ms, charge_empty_wait_ms(g.percent, g.ua, board->charger.cell_mah), now);
	return !power_awake() && next < CHARGER_TASK_MS ? CHARGER_TASK_MS : next;
}

static uint32_t fan_percent(void)
{
	struct hub_gauge g;
	if (full && cable_now)
		return 100;
	return gauge(&g) ? g.percent : charge_percent_from_mv(pmic_battery_mv());
}

static bool fan_charging(void)
{
	uint8_t s[2];
	uint8_t reg = STAT0;
	return ready && cable_now && !eoc && !i2c_xfer(BUS, ADDR, &reg, 1, s, 2) &&
	       ((s[0] & STAT0_STAT) || (s[1] & STAT1_TOCHG));
}

static bool fan_cable(void) { return cable_now; }

static void fan_cable_irq_init(void)
{
	gpio_irq_config(board->charger.irq, GPIO_IRQ_EDGE_FALLING, irq_edge);
	gpio_irq_enable(board->charger.irq, true);
	if (board->charger.mpm)
		wake_pin(board->charger.mpm, true, false);
}

static bool fan_cable_irq(void)
{
	if (irq_seen)
		fan_poll();
	bool changed = cable_now != cable_told;
	cable_told = cable_now;
	return changed;
}

static uint32_t fan_mv(void)
{
	struct hub_gauge g;
	return gauge(&g) ? g.mv : pmic_battery_mv();
}

static int32_t fan_ua(void)
{
	struct hub_gauge g;
	return gauge(&g) ? g.ua : 0;
}

static uint32_t fan_ua_age_ms(void)
{
	struct hub_gauge g;
	return gauge(&g) ? time_ms() - g.t_ms : UINT32_MAX;
}

static const char *fan_status(void)
{
	static char s[128];
	struct hub_gauge g = { 0 };
	bool have = gauge(&g);
	snprintf(s, sizeof s, "usb %u charging %u %s%s%s, %s %lu%% %lu mV %d.%d C, %lu errors", cable_now, fan_charging(),
		 full ? "full" : eoc ? "ended" : cv ? "cv" : "cc", warm ? " warm" : "", stepped ? " stepped" : "",
		 have ? "gauge" : "no gauge", fan_percent(), fan_mv(), g.decidegrees / 10,
		 (g.decidegrees < 0 ? -g.decidegrees : g.decidegrees) % 10, errors);
	return s;
}

const struct battery_driver battery_fan54511 = {
	.init = fan_init,
	.poll = fan_poll,
	.next_ms = fan_next_ms,
	.percent = fan_percent,
	.charging = fan_charging,
	.cable = fan_cable,
	.usb = fan_cable,
	.cable_irq_init = fan_cable_irq_init,
	.cable_irq = fan_cable_irq,
	.mv = fan_mv,
	.ua = fan_ua,
	.ua_age_ms = fan_ua_age_ms,
	.status = fan_status,
};
