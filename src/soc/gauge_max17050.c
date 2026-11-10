/* smelt's battery: a MAX17050 gauge on I2C and a wireless receiver that
 * charges the cell, which charge_policy tells to stop through its
 * termination line. The gauge keeps the cell model the vendor loaded
 * into it, and is left alone until its status says it lost power and the
 * model with it; the board's copy of the model is then written back the
 * way the vendor's driver does it, without which the percentage means
 * nothing and charging stays off. The PMIC's charger is not touched; of its registers only the USB path's
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

#define BUS		(board->gauge.i2c.bus)
#define ADDR		(board->gauge.i2c.addr)
#define GPIO_DOCK	(board->gauge.dock)
#define MPM_PIN_DOCK	(board->gauge.dock_mpm)
#define GPIO_CHARGE_N	(board->gauge.charge_n)
#define GPIO_CHARGE_TERM	(board->gauge.charge_term)
#define GPIO_CHARGE_RATE	(board->gauge.charge_rate)
#define BUS_HZ		200000
#define POLL_MS		30000
#define DOCK_DEBOUNCE_MS	2000
#define DOCK_PULSE_MS	1500

static bool ready, wake_armed, dock_state, dock_settling, dock_plugged;
static volatile bool dock_irq;
static volatile uint32_t pulse_t;
static volatile bool pulsed;
static uint32_t soc = 50, poll_t, dock_t, usb_gone_t;
static bool usb_was;
#define USB_GONE_MS	1000
static uint16_t last_status;
static unsigned errors;
static struct charge_policy policy;

static int rd(uint8_t reg, uint16_t *v)
{
	uint8_t b[2];
	int r = i2c_xfer(BUS, ADDR, &reg, 1, b, 2);
	if (r) {
		errors++;
		return r;
	}
	*v = (uint16_t)(b[0] | b[1] << 8);
	return 0;
}

static int wr(uint8_t reg, uint16_t v)
{
	uint8_t b[3] = { reg, (uint8_t)v, (uint8_t)(v >> 8) };
	int r = i2c_xfer(BUS, ADDR, b, 3, NULL, 0);
	if (r)
		errors++;
	return r;
}

/* Written until it reads back, as the vendor's driver does. */
static int wr_verify(uint8_t reg, uint16_t v)
{
	uint16_t back = ~v;
	for (int i = 0; i < 8; i++) {
		if (!wr(reg, v) && !rd(reg, &back) && back == v)
			return 0;
	}
	return -1;
}

static void wait_ms(unsigned ms)
{
	for (; ms; ms -= 10) {
		delay_ms(10);
		wdt_feed();
	}
}

/* The vendor driver's start after a power loss: its register overrides,
 * the configuration, the characterisation table under the model lock,
 * the cell's own parameters and capacities, then the state of charge
 * seeded from the voltage-only estimate and the lost-power flag cleared.
 * The model is checked word for word and must read as zeros once
 * locked; a step that does not take leaves the flag set, so the
 * percentage stays untrusted and the load is tried again later. */
static bool model_load(void)
{
	const struct max17050_model *m = board->gauge.model;
	uint16_t v, vfsoc, status;
	int r = wr(MAX17050_TGAIN, m->tgain) | wr(MAX17050_TOFF, m->toff) | wr(0x01, 0xff00) | wr(0x02, 0x7f80) |
		wr(MAX17050_CONFIG, m->config) | wr(MAX17050_DESIGNCAP, m->design_cap) |
		wr(MAX17050_ICHGTERM, m->ichgt_term) | wr(MAX17050_LEARNCFG, m->learn_cfg) |
		wr(MAX17050_FILTERCFG, m->filter_cfg) | wr(MAX17050_RELAXCFG, m->relax_cfg) |
		wr(MAX17050_FULLCAP, m->fullcap) | wr(MAX17050_FULLCAPNOM, m->fullcapnom);
	wait_ms(500);
	r |= wr(MAX17050_CONFIG, m->config) | wr(MAX17050_LEARNCFG, m->learn_cfg) | wr(MAX17050_FILTERCFG, m->filter_cfg) |
	     wr(MAX17050_RELAXCFG, m->relax_cfg) | wr(MAX17050_FULLSOCTHR, m->full_soc_thresh);
	r |= wr(MAX17050_MLOCK1, MAX17050_MLOCK1_UNLOCK) | wr(MAX17050_MLOCK2, MAX17050_MLOCK2_UNLOCK);
	for (unsigned i = 0; i < MAX17050_MODEL_WORDS; i++)
		r |= wr((uint8_t)(MAX17050_MODEL + i), m->cell[i]);
	for (unsigned i = 0; i < MAX17050_MODEL_WORDS; i++)
		if (rd((uint8_t)(MAX17050_MODEL + i), &v) || v != m->cell[i])
			r = -1;
	r |= wr(MAX17050_MLOCK1, 0) | wr(MAX17050_MLOCK2, 0);
	for (unsigned i = 0; i < MAX17050_MODEL_WORDS; i++)
		if (rd((uint8_t)(MAX17050_MODEL + i), &v) || v)
			r = -1;
	if (r) {
		log_printf("gauge: cell model not taken\n");
		return false;
	}
	r = wr_verify(MAX17050_RCOMP0, m->rcomp0) | wr_verify(MAX17050_TEMPCO, m->tempco) |
	    wr_verify(MAX17050_ICHGTERM, m->ichgt_term) | wr_verify(MAX17050_QRTBL00, m->qrtbl[0]) |
	    wr_verify(MAX17050_QRTBL10, m->qrtbl[1]) | wr_verify(MAX17050_QRTBL20, m->qrtbl[2]) |
	    wr_verify(MAX17050_QRTBL30, m->qrtbl[3]);
	r |= wr_verify(MAX17050_FULLCAP, m->fullcap) | wr(MAX17050_DESIGNCAP, m->design_cap) |
	     wr_verify(MAX17050_FULLCAPNOM, m->fullcapnom);
	wait_ms(350);
	r |= rd(MAX17050_VFSOC, &vfsoc);
	r |= wr(MAX17050_VFSOC0_ENABLE, MAX17050_VFSOC0_UNLOCK) | wr_verify(MAX17050_VFSOC0, vfsoc) |
	     wr(MAX17050_VFSOC0_ENABLE, 0);
	r |= wr_verify(MAX17050_CYCLES, MAX17050_INIT_CYCLES);
	uint16_t remcap = max17050_remcap(vfsoc, m->fullcapnom);
	r |= wr_verify(MAX17050_REMCAP, remcap) | wr_verify(MAX17050_REPCAP, remcap) |
	     wr_verify(MAX17050_DQACC, max17050_dqacc(m->fullcap)) | wr_verify(MAX17050_DPACC, MAX17050_DPACC_200) |
	     wr_verify(MAX17050_FULLCAP, m->fullcap) | wr(MAX17050_DESIGNCAP, m->design_cap) |
	     wr_verify(MAX17050_FULLCAPNOM, m->fullcapnom) | wr(MAX17050_REPSOC, vfsoc);
	r |= rd(MAX17050_STATUS, &status);
	if (!r)
		r = wr(MAX17050_STATUS, status & ~0x0002) | wr(MAX17050_CONFIG_VER, m->version);
	log_printf("gauge: cell model %s, %u%% by voltage\n", r ? "not fully loaded" : "loaded", vfsoc >> 8);
	return !r;
}

/* The gauge started over, as the vendor's driver does it: the model
 * locked and the flag cleared, then the reset command until the flag
 * is back. */
static bool soft_reset(void)
{
	uint16_t v, a = 1, b = 1;
	for (unsigned i = 0; i < 8 && (a || b); i++) {
		wr(MAX17050_MLOCK1, 0);
		wr(MAX17050_MLOCK2, 0);
		if (!rd(MAX17050_STATUS, &v))
			wr(MAX17050_STATUS, v & ~0x0002);
		if (rd(MAX17050_MLOCK1, &a) || rd(MAX17050_MLOCK2, &b))
			a = 1;
	}
	if (a || b)
		return false;
	for (unsigned i = 0; i < 8; i++) {
		wr(MAX17050_VFSOC0_ENABLE, 0x000f);
		delay_ms(3);
		if (!rd(MAX17050_STATUS, &v) && (v & 0x0002))
			return true;
	}
	return false;
}

/* USB taken out, for long enough to be meant. */
static void usb_check(uint32_t now)
{
	bool usb = vbus_present();
	if (usb)
		usb_gone_t = 0;
	else if (usb_was)
		usb_gone_t = now | 1;
	usb_was = usb;
	if (!usb_gone_t || now - usb_gone_t < USB_GONE_MS)
		return;
	usb_gone_t = 0;
	if (!ready || !board->gauge.model)
		return;
	bool ok = soft_reset();
	log_printf("gauge: usb out, %s\n", ok ? "started over" : "would not start over");
	if (ok) {
		model_load();
		poll_t = now - POLL_MS;
	}
}

/* For the console: the model written again, whatever the status. */
bool gauge_model_load(void)
{
	return ready && board->gauge.model && model_load();
}

/* Loads the model when the status says it is gone. */
static void model_check(void)
{
	uint16_t status;
	if (!ready || !board->gauge.model || rd(MAX17050_STATUS, &status) || !max17050_lost_model(status))
		return;
	log_printf("gauge: lost its cell model, loading it\n");
	model_load();
}

/* On the pad the receiver holds its line low. An older one has a line
 * that is high while it charges and another that is high with it and
 * pulsed high while it sits on the pad without charging: there the
 * watch is off the pad once neither has been seen for a while. */
static bool dock_line(void)
{
	if (!board->gauge.dock_pulses)
		return !gpio_get(GPIO_DOCK);
	return gpio_get(board->gauge.charging) || gpio_get(GPIO_DOCK) || (pulsed && time_ms() - pulse_t < DOCK_PULSE_MS);
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
	gpio_set(GPIO_CHARGE_TERM, stop);
	if (GPIO_CHARGE_RATE)
		gpio_set(GPIO_CHARGE_RATE, reduced);
	if (!driven) {
		driven = true;
		gpio_config(GPIO_CHARGE_TERM, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
		if (GPIO_CHARGE_RATE)
			gpio_config(GPIO_CHARGE_RATE, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
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
		power_off_arm();
		reboot();
	}
}

static void gauge_init(void)
{
	gpio_config(GPIO_DOCK, 0, GPIO_INPUT, GPIO_PULL_UP, 2);
	if (board->gauge.dock_pulses)
		gpio_config(board->gauge.charging, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	/* The factory kill circuit disconnects the battery while USB powers
	 * the watch and keeps it so until it is disarmed, so the first unplug
	 * cuts the power. The bootloader leaves the pin low (armed); the
	 * vendor kernel drives it high unless a factory cable is in. */
	if (board->gauge.kill_disable) {
		gpio_config(board->gauge.kill_disable, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
		gpio_set(board->gauge.kill_disable, 1);
	}
	uint16_t id = 0;
	int r = i2c_init(BUS, board->gauge.i2c.sda, board->gauge.i2c.scl, board->gauge.i2c.func, BUS_HZ);
	dock_state = dock_line();
	if (!r)
		r = rd(MAX17050_DEVNAME, &id);
	if (r || id != MAX17050_DEVNAME_ID) {
		log_printf("gauge: not found (%d, id %04x)\n", r, id);
		charge_lines(true, true);
		return;
	}
	ready = true;
	model_check();
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
	/* A pulsed line's edges are what keeps it at its level. */
	if (!dock_settling || (edge && !board->gauge.dock_pulses)) {
		dock_settling = true;
		dock_t = now;
		return;
	}
	if (now - dock_t < DOCK_DEBOUNCE_MS)
		return;
	dock_settling = false;
	dock_state = line;
	dock_plugged = line;
	log_printf("gauge: dock %u, usb %u\n", line, vbus_present());
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
		wake_pin(MPM_PIN_DOCK, arm, false);
	}
	dock_update(now);
	if (board->gauge.usb_disturbs)
		usb_check(now);
	if (now - poll_t < POLL_MS)
		return;
	poll_t = now;
	model_check();
	refresh();
	charge_check();
}

static uint32_t gauge_percent(void) { return soc; }
static bool gauge_charging(void) { return docked(); }
static bool gauge_cable(void) { return docked() || vbus_present(); }

static bool gauge_usb(void) { return vbus_present(); }

/* On the pad the charge limits need a look on time, asleep too, and so
 * does a dock line that changed. */
static uint32_t gauge_next_ms(void)
{
	uint32_t now = time_ms(), next = UINT32_MAX;
	if (dock_irq)
		return 1;
	if (board->gauge.dock_pulses && dock_state && !gpio_get(board->gauge.charging) && !gpio_get(GPIO_DOCK))
		next = ms_left(pulse_t, DOCK_PULSE_MS, now);
	if (dock_settling)
		next = ms_left(dock_t, DOCK_DEBOUNCE_MS, now);
	if (usb_gone_t) {
		uint32_t gone = ms_left(usb_gone_t, USB_GONE_MS, now);
		if (gone < next)
			next = gone;
	}
	if (docked()) {
		uint32_t poll = ms_left(poll_t, POLL_MS, now);
		if (poll < next)
			next = poll;
	}
	return next;
}

static void dock_edge(unsigned n)
{
	gpio_irq_ack(n);
	pulse_t = time_ms();
	pulsed = true;
	dock_irq = true;
}

static void charging_edge(unsigned n)
{
	gpio_irq_ack(n);
	dock_irq = true;
}

/* The dock line, and the PMIC's USB path for the pogo pins, which the
 * dock line does not see. */
static void gauge_cable_irq_init(void)
{
	gpio_irq_config(GPIO_DOCK, GPIO_IRQ_EDGE_FALLING, dock_edge);
	gpio_irq_enable(GPIO_DOCK, true);
	/* Put on the pad, the pulsed line only rises. */
	if (board->gauge.dock_pulses) {
		gpio_irq_config(board->gauge.charging, GPIO_IRQ_EDGE_RISING, charging_edge);
		gpio_irq_enable(board->gauge.charging, true);
	}
	vbus_irq_init();
}

/* The dock only once the line has settled low; USB on its own edge. */
static bool gauge_cable_irq(void)
{
	bool fired = dock_plugged;
	dock_plugged = false;
	return vbus_irq() || fired;
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
		 vbus_present(), board->gauge.dock_pulses ? !gpio_get(board->gauge.charging) : gpio_get(GPIO_CHARGE_N), soc, c / 10, (c < 0 ? -c : c) % 10, last_status,
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
