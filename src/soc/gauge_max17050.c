/* smelt's battery: a MAX17050 gauge on I2C and a wireless receiver that
 * charges the cell, which charge_policy tells to stop through its
 * termination line. Nothing here writes to the gauge: the cell model in
 * it is the vendor's, its driver only loads it after the gauge lost
 * power. The PMIC's charger is not touched; of its registers only the USB path's
 * status is read, which is how the vendor sees USB too.
 *
 * On the charger: the dock line is low while on the pad, and as for the
 * vendor, that counts as charging. USB through the pogo pins counts as a
 * cable too, but the pad does not bring up USB. The line glitches, so it
 * only counts once it has held its level for DOCK_DEBOUNCE_MS. */
#include "asteroid.h"
#include "present.h"
#include "soc/max17050.h"
#include "soc/charge_policy.h"

#define QUP		(board->gauge.i2c.qup)
#define ADDR		(board->gauge.i2c.addr)
#define GPIO_DOCK	(board->gauge.dock)
#define MPM_PIN_DOCK	(board->gauge.dock_mpm)
#define GPIO_CHARGE_N	(board->gauge.charge_n)
#define GPIO_CHARGE_TERM	(board->gauge.charge_term)
#define GPIO_CHARGE_RATE	(board->gauge.charge_rate)
#define BUS_HZ		200000
#define POLL_MS		30000
#define DOCK_DEBOUNCE_MS	2000

static bool ready, wake_armed, dock_state, dock_settling, dock_plugged;
static volatile bool dock_irq;
static uint32_t soc = 50, poll_t, dock_t;
static uint16_t last_status;
static unsigned errors;
static struct charge_policy policy;

static int rd(uint8_t reg, uint16_t *v)
{
	uint8_t b[2];
	int r = i2c_qup_xfer(QUP, ADDR, &reg, 1, b, 2);
	if (r) {
		errors++;
		return r;
	}
	*v = (uint16_t)(b[0] | b[1] << 8);
	return 0;
}

static bool dock_line(void)
{
	return !tlmm_get(GPIO_DOCK);
}

static bool docked(void)
{
	return dock_state;
}

static void refresh(void)
{
	uint16_t v;
	if (!ready)
		return;
	if (!rd(MAX17050_REPSOC, &v))
		soc = max17050_percent(v);
	if (!rd(MAX17050_STATUS, &v))
		last_status = v;
}

/* The lines stay as the bootloader left them until the first decision:
 * a stop, however short, can turn the pad off until the watch is lifted. */
static void charge_lines(bool stop, bool reduced)
{
	static bool driven;
	tlmm_set(GPIO_CHARGE_TERM, stop);
	tlmm_set(GPIO_CHARGE_RATE, reduced);
	if (!driven) {
		driven = true;
		tlmm_config(GPIO_CHARGE_TERM, 0, TLMM_OUTPUT, TLMM_PULL_NONE, 2);
		tlmm_config(GPIO_CHARGE_RATE, 0, TLMM_OUTPUT, TLMM_PULL_NONE, 2);
	}
}

/* Anything unknown about the cell or the gauge stops charging. */
static void charge_check(void)
{
	uint16_t t = 0, c = 0, level = 0, status = 0, v = 0;
	bool valid = ready && !rd(MAX17050_TEMP, &t) && !rd(MAX17050_REPSOC, &level) && !rd(MAX17050_STATUS, &status) &&
		     !rd(MAX17050_VCELL, &v) && !max17050_no_battery(status) && !max17050_lost_model(status);
	int temp = max17050_decidegrees(t);
	bool current_valid = ready && !rd(MAX17050_AVGCURRENT, &c);
	struct charge_input in = {
		.now_ms = time_ms(), .valid = valid && charge_temp_plausible(temp), .current_valid = current_valid,
		.temp = temp, .percent = max17050_percent(level), .mv = max17050_mv(v),
		.charge_ma = current_valid ? -max17050_ua(c, board->rsense_uohm) / 1000 : 0,
	};
	bool was = policy.stopped, rate = policy.reduced, stop = charge_policy_step(&policy, &in);
	charge_lines(stop, policy.reduced);
	if (stop != was || policy.reduced != rate)
		log_printf("gauge: charging %s%s at %d.%d C, %lu mV, %lu%%%s\n", stop ? "stopped" : "allowed",
			   policy.reduced ? ", reduced" : "", temp / 10, (temp < 0 ? -temp : temp) % 10, in.mv, in.percent,
			   in.valid ? "" : ", gauge not trusted");
	if (in.valid && temp >= CHARGE_POWER_OFF_TEMP) {
		log_printf("gauge: %d.%d C, powering off\n", temp / 10, temp % 10);
		pmic_shutdown_mode();
		reboot();
	}
}

static void gauge_init(void)
{
	tlmm_config(GPIO_DOCK, 0, TLMM_INPUT, TLMM_PULL_UP, 2);
	/* The factory kill circuit disconnects the battery while USB powers
	 * the watch and keeps it so until it is disarmed, so the first unplug
	 * cuts the power. The bootloader leaves the pin low (armed); the
	 * vendor kernel drives it high unless a factory cable is in. */
	if (board->gauge.kill_disable) {
		tlmm_config(board->gauge.kill_disable, 0, TLMM_OUTPUT, TLMM_PULL_NONE, 2);
		tlmm_set(board->gauge.kill_disable, 1);
	}
	uint16_t id = 0;
	int r = i2c_qup_init(QUP, board->gauge.i2c.sda, board->gauge.i2c.scl, board->gauge.i2c.func, BUS_HZ);
	dock_state = dock_line();
	if (!r)
		r = rd(MAX17050_DEVNAME, &id);
	if (r || id != MAX17050_DEVNAME_ID) {
		log_printf("gauge: not found (%d, id %04x)\n", r, id);
		charge_lines(true, true);
		return;
	}
	ready = true;
	refresh();
	log_printf("gauge: max17050, %lu%%, status %04x%s, dock %u\n", soc, last_status,
		   max17050_lost_model(last_status) ? ", lost its cell model" : "", docked());
	poll_t = time_ms();
	charge_check();
}

/* A falling edge seen since the last look starts the wait again, as the
 * line may have come back between looks. */
static void dock_update(uint32_t now)
{
	bool edge = dock_irq, line = dock_line();
	dock_irq = false;
	if (line == dock_state) {
		dock_settling = false;
		return;
	}
	if (!dock_settling || edge) {
		dock_settling = true;
		dock_t = now;
		return;
	}
	if (now - dock_t < DOCK_DEBOUNCE_MS)
		return;
	dock_settling = false;
	dock_state = line;
	dock_plugged = line;
	log_printf("gauge: dock %u, usb %u\n", line, pmic_usb_present());
	present_clock_soon();
}

/* The MPM sees levels, so its wake-up on the dock line is armed only
 * while the line is high, or the watch would never stay asleep with it
 * low, on the dock or waiting out the debounce. */
static void gauge_poll(void)
{
	bool arm = !dock_line();
	uint32_t now = time_ms();
	if (arm != wake_armed) {
		wake_armed = arm;
		spm_wake_pin(MPM_PIN_DOCK, arm, false);
	}
	dock_update(now);
	if (now - poll_t < POLL_MS)
		return;
	poll_t = now;
	refresh();
	charge_check();
}

static uint32_t gauge_percent(void) { return soc; }
static bool gauge_charging(void) { return docked(); }
static bool gauge_cable(void) { return docked() || pmic_usb_present(); }

static bool gauge_usb(void) { return pmic_usb_present(); }

/* On the pad the charge limits need a look on time, asleep too, and so
 * does a dock line that changed. */
static uint32_t gauge_next_ms(void)
{
	uint32_t now = time_ms(), next = UINT32_MAX;
	if (dock_irq)
		return 1;
	if (dock_settling)
		next = ms_left(dock_t, DOCK_DEBOUNCE_MS, now);
	if (docked()) {
		uint32_t poll = ms_left(poll_t, POLL_MS, now);
		if (poll < next)
			next = poll;
	}
	return next;
}

static void dock_edge(unsigned n)
{
	tlmm_irq_ack(n);
	dock_irq = true;
}

/* The dock line, and the PMIC's USB path for the pogo pins, which the
 * dock line does not see. */
static void gauge_cable_irq_init(void)
{
	tlmm_irq_config(GPIO_DOCK, TLMM_IRQ_EDGE_FALLING, dock_edge);
	tlmm_irq_enable(GPIO_DOCK, true);
	pmic_usb_irq_init();
}

/* The dock only once the line has settled low; USB on its own edge. */
static bool gauge_cable_irq(void)
{
	bool fired = dock_plugged;
	dock_plugged = false;
	return pmic_usb_irq() || fired;
}

static uint32_t gauge_mv(void)
{
	uint16_t v;
	return ready && !rd(MAX17050_VCELL, &v) ? max17050_mv(v) : 0;
}

static int32_t current(uint8_t reg)
{
	uint16_t v;
	return ready && !rd(reg, &v) ? max17050_ua(v, board->rsense_uohm) : 0;
}

static int32_t gauge_ua(void) { return current(MAX17050_CURRENT); }
/* The gauge's own running average, not one since the last call. */
static int32_t gauge_avg_ua(void) { return current(MAX17050_AVGCURRENT); }

static const char *gauge_status(void)
{
	static char s[96];
	uint16_t t = 0;
	if (!ready)
		return "no gauge";
	int r = rd(MAX17050_TEMP, &t), c = r ? 0 : max17050_decidegrees(t);
	snprintf(s, sizeof s, "dock %u usb %u chg_n %u, %lu%%, %d.%d C, status %04x%s, %u errors", docked(),
		 pmic_usb_present(), tlmm_get(GPIO_CHARGE_N), soc, c / 10, (c < 0 ? -c : c) % 10, last_status,
		 max17050_no_battery(last_status) ? " no battery" : "", errors);
	return s;
}

const struct battery_driver battery_max17050 = {
	.init = gauge_init,
	.poll = gauge_poll,
	.next_ms = gauge_next_ms,
	.percent = gauge_percent,
	.charging = gauge_charging,
	.cable = gauge_cable,
	.usb = gauge_usb,
	.cable_irq_init = gauge_cable_irq_init,
	.cable_irq = gauge_cable_irq,
	.mv = gauge_mv,
	.ua = gauge_ua,
	.avg_ua = gauge_avg_ua,
	.status = gauge_status,
};
