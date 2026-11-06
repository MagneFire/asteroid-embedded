/* The limits are healthd.smelt's: above 45 C charging runs in cycles of
 * 10 minutes on and 5 off, at 53 C it stops, the rate is reduced from
 * 4.1 V down to 3.9 V, and charging is complete after 30 minutes at full
 * or 5 readings of 16 mA or less, until the level drops to 98 %. Unlike
 * the vendor's, the limits apply from boot. Stopping the receiver blips
 * the pad line, so the pad line plays no part here. */
#include "charge_policy.h"

#define WARM_TEMP	450
#define WARM_HYST	30
#define HOT_TEMP	530
#define HOT_HYST	10
#define CYCLE_ON_MS	600000u
#define CYCLE_OFF_MS	300000u
#define FULL_TIMEOUT_MS	1800000u
#define FULL_MA		16
#define FULL_READINGS	5
#define RESTART_PERCENT	98
#define REDUCE_MV	4100
#define RESTORE_MV	3900
#define TEMP_MIN	(-200)
#define TEMP_MAX	800

bool charge_temp_plausible(int temp)
{
	return temp > TEMP_MIN && temp < TEMP_MAX;
}

bool charge_policy_step(struct charge_policy *p, const struct charge_input *in)
{
	uint32_t now = in->now_ms;
	if (!in->valid) {
		p->stopped = true;
		p->reduced = true;
		return true;
	}
	if (in->temp >= HOT_TEMP)
		p->hot = true;
	else if (in->temp < HOT_TEMP - HOT_HYST)
		p->hot = false;
	bool warm = in->temp >= WARM_TEMP || (p->warm && in->temp >= WARM_TEMP - WARM_HYST);
	if (warm && !p->warm)
		p->cycle_ms = now;
	p->warm = warm;
	if (in->mv > REDUCE_MV)
		p->reduced = true;
	else if (in->mv <= RESTORE_MV)
		p->reduced = false;
	bool off_cycle = p->warm && (now - p->cycle_ms) % (CYCLE_ON_MS + CYCLE_OFF_MS) >= CYCLE_ON_MS;
	bool held = p->hot || off_cycle;
	if (in->percent <= RESTART_PERCENT)
		p->complete = false;
	if (in->percent < 100) {
		p->full = false;
		p->low_current = 0;
	} else if (!p->full) {
		p->full = true;
		p->full_ms = now;
	}
	if (p->full && !p->complete && !held) {
		if (in->current_valid)
			p->low_current = in->charge_ma >= 0 && in->charge_ma <= FULL_MA ? p->low_current + 1 : 0;
		if (p->low_current >= FULL_READINGS || now - p->full_ms >= FULL_TIMEOUT_MS)
			p->complete = true;
	}
	p->stopped = held || p->complete;
	return p->stopped;
}
