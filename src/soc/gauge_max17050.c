/* smelt's battery: a MAX17050 gauge on I2C and a wireless receiver that
 * charges the cell by itself. Nothing here writes to the gauge or drives
 * the receiver's control lines: the cell model in the gauge is the
 * vendor's, its driver only loads it after the gauge lost power, and the
 * receiver charges with the lines as the bootloader leaves them. The
 * PMIC's charger is not touched; of its registers only the USB path's
 * status is read, which is how the vendor sees USB too.
 *
 * On the charger: the dock line is low while on the pad, and as for the
 * vendor, that counts as charging. USB through the pogo pins counts as a
 * cable too, but the pad does not bring up USB. */
#include "asteroid.h"
#include "soc/max17050.h"

#define QUP		(board->gauge.i2c.qup)
#define ADDR		(board->gauge.i2c.addr)
#define GPIO_DOCK	(board->gauge.dock)
#define MPM_PIN_DOCK	(board->gauge.dock_mpm)
#define GPIO_CHARGE_N	(board->gauge.charge_n)
#define BUS_HZ		200000
#define POLL_MS		30000

static bool ready, wake_armed;
static volatile bool dock_irq;
static uint32_t soc = 50, poll_t;
static uint16_t last_status;
static unsigned errors;

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

static bool docked(void)
{
	return !tlmm_get(GPIO_DOCK);
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

static void gauge_init(void)
{
	tlmm_config(GPIO_DOCK, 0, TLMM_INPUT, TLMM_PULL_UP, 2);
	uint16_t id = 0;
	int r = i2c_qup_init(QUP, board->gauge.i2c.sda, board->gauge.i2c.scl, board->gauge.i2c.func, BUS_HZ);
	if (!r)
		r = rd(MAX17050_DEVNAME, &id);
	if (r || id != MAX17050_DEVNAME_ID) {
		log_printf("gauge: not found (%d, id %04x)\n", r, id);
		return;
	}
	ready = true;
	refresh();
	log_printf("gauge: max17050, %lu%%, status %04x%s, dock %u\n", soc, last_status,
		   max17050_lost_model(last_status) ? ", lost its cell model" : "", docked());
	poll_t = time_ms();
}

/* The MPM sees levels, so its wake-up on the dock line is armed only
 * while off the dock, or the watch would never stay asleep on it. */
static void gauge_poll(void)
{
	bool arm = !docked();
	if (arm != wake_armed) {
		wake_armed = arm;
		spm_wake_pin(MPM_PIN_DOCK, arm, false);
	}
	uint32_t now = time_ms();
	if (now - poll_t < POLL_MS)
		return;
	poll_t = now;
	refresh();
}

static uint32_t gauge_percent(void) { return soc; }
static bool gauge_charging(void) { return docked(); }
static bool gauge_cable(void) { return docked() || pmic_usb_present(); }

static bool gauge_usb(void) { return pmic_usb_present(); }

static void dock_edge(unsigned n)
{
	tlmm_irq_ack(n);
	dock_irq = true;
}

static void gauge_cable_irq_init(void)
{
	tlmm_irq_config(GPIO_DOCK, TLMM_IRQ_EDGE_FALLING, dock_edge);
	tlmm_irq_enable(GPIO_DOCK, true);
}

static bool gauge_cable_irq(void)
{
	bool fired = dock_irq;
	dock_irq = false;
	return fired;
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
