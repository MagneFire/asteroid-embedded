/* When smelt's wireless receiver may charge, as the vendor's healthd
 * decides it. No hardware involved. */
#ifndef CHARGE_POLICY_H
#define CHARGE_POLICY_H
#include <stdbool.h>
#include <stdint.h>

/* Tenths of a degree; above this the watch powers off. */
#define CHARGE_POWER_OFF_TEMP	600

struct charge_policy {
	bool hot, warm, full, complete, stopped, reduced;
	uint32_t cycle_ms, full_ms;
	unsigned low_current;
};

struct charge_input {
	uint32_t now_ms;
	/* The gauge answered and has a good cell and temperature. */
	bool valid;
	bool current_valid;
	int temp;		/* tenths of a degree */
	uint32_t percent, mv;
	int32_t charge_ma;	/* into the cell */
};

/* Whether charging should stop now; p->reduced says the rate. */
bool charge_policy_step(struct charge_policy *p, const struct charge_input *in);
/* Whether a gauge temperature can be real. */
bool charge_temp_plausible(int temp);
#endif
