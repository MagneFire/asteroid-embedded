/* dory's battery: the PM8226 charges the cell over USB and a MAX17050
 * measures it. The gauge is set to take the cell's temperature from the
 * host, which the vendor reads at the PMIC's thermistor input and writes
 * in, so each look writes it too. The charger's own hot and cold limits
 * are off on this watch; the vendor's thermal table charges at 4.1 V and
 * 200 mA from 45 C, stops from 55 C and below -10 C, and goes back to
 * normal under 43 C and from -5 C. Its charger also takes 200 mA off
 * above 4.25 V, until the cell is under 4.05 V again. With no reading of
 * the thermistor, charging is held off. */
#include "asteroid.h"
#include "soc/max17050.h"
#include "soc/charge_policy.h"

#define POLL_MS		10000
#define WARM		450
#define HOT_STOP	550
#define HOT_RESUME	430
#define COLD_STOP	(-100)
#define COLD_RESUME	(-50)
#define WARM_MV		4100
#define WARM_MA		200
#define STEP_DOWN_MV	4250
#define STEP_UP_MV	4050
#define STEP_DOWN_MA	200

static bool too_hot, too_cold, warm, stepped, unread, cable;
static uint32_t poll_t;

static void limits(void)
{
	uint16_t ma = board->chg.ibat_max_ma;
	uint32_t mv = max17050_cell_mv();
	stepped = stepped ? mv >= STEP_UP_MV : mv >= STEP_DOWN_MV;
	if (stepped)
		ma = (uint16_t)(ma - STEP_DOWN_MA);
	if (warm && ma > WARM_MA)
		ma = WARM_MA;
	pmic_charger_limits(warm ? WARM_MV : 0, ma == board->chg.ibat_max_ma ? 0 : ma);
}

static void temperature(void)
{
	int t;
	if (!pmic_battery_temp(&t)) {
		if (!unread)
			log_printf("battery: no temperature, charging held off\n");
		unread = true;
		pmic_charger_inhibit(true);
		return;
	}
	unread = false;
	max17050_host_temp(t);
	bool hot = too_hot ? t >= HOT_RESUME : t >= HOT_STOP;
	bool cold = too_cold ? t < COLD_RESUME : t <= COLD_STOP;
	bool w = warm ? t >= HOT_RESUME : t >= WARM;
	if (hot != too_hot || cold != too_cold || w != warm)
		log_printf("battery: %d.%d C, charging %s\n", t / 10, (t < 0 ? -t : t) % 10,
			   hot || cold ? "held off" : w ? "reduced" : "allowed");
	too_hot = hot;
	too_cold = cold;
	warm = w;
	pmic_charger_inhibit(hot || cold);
	limits();
	if (t >= CHARGE_POWER_OFF_TEMP) {
		log_printf("battery: %d.%d C, powering off\n", t / 10, t % 10);
		power_off_arm();
		reboot();
	}
}

static void pg_init(void)
{
	pmic_charger_inhibit(true);
	pmic_charger_configure();
	bool found = max17050_probe();
	temperature();
	poll_t = time_ms();
	uint16_t st = max17050_last_status();
	if (found)
		log_printf("battery: max17050, %lu%%, status %04x%s\n", max17050_soc(), st,
			   max17050_lost_model(st) ? ", lost its cell model" : "");
}

/* The vendor scales the gauge's 99 % to full, so the charger resumes
 * below that rather than at the first dip under 100 %. */
static void pg_poll(void)
{
	uint32_t now = time_ms();
	bool c = vbus_present();
	if (c && !cable)
		poll_t = now - POLL_MS;
	cable = c;
	if (now - poll_t >= POLL_MS) {
		poll_t = now;
		temperature();
		max17050_update();
	}
	uint32_t soc = max17050_soc() * 100 / 99;
	pmic_charger_poll(soc > 100 ? 100 : soc);
}

/* Only on the cable: off it, a look on the next wake is enough. */
static uint32_t pg_next_ms(void)
{
	if (!cable)
		return UINT32_MAX;
	uint32_t gone = time_ms() - poll_t;
	return gone >= POLL_MS ? 0 : POLL_MS - gone;
}

static const char *pg_status(void)
{
	static char s[160];
	int t = 0;
	bool read = pmic_battery_temp(&t);
	uint16_t st = max17050_last_status();
	if (!read)
		snprintf(s, sizeof s, "%s; gauge %lu%%, no temperature, status %04x, %u errors", pmic_charger_status(),
			 max17050_soc(), st, max17050_errors());
	else
		snprintf(s, sizeof s, "%s; gauge %lu%%, %d.%d C, status %04x, %u errors", pmic_charger_status(),
			 max17050_soc(), t / 10, (t < 0 ? -t : t) % 10, st, max17050_errors());
	return s;
}

const struct battery_driver battery_pmic_max17050 = {
	.init = pg_init,
	.poll = pg_poll,
	.next_ms = pg_next_ms,
	.percent = max17050_soc,
	.charging = pmic_charging,
	.cable = vbus_present,
	.cable_irq_init = vbus_irq_init,
	.cable_irq = vbus_irq,
	.mv = max17050_cell_mv,
	.ua = max17050_cell_ua,
	.avg_ua = max17050_cell_avg_ua,
	.status = pg_status,
};
