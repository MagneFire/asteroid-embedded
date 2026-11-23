/* ST's gas gauge algorithm for the STC3117 (version 2.03, as the vendor
 * kernel carries it), on the chip's registers through two calls, no
 * hardware involved. */
#ifndef STC3117_H
#define STC3117_H
#include <stdbool.h>
#include <stdint.h>

#define STC3117_ADDR		0x70
#define STC3117_REG_MODE	0x00
#define STC3117_REG_CTRL	0x01
#define STC3117_REG_SOC		0x02
#define STC3117_REG_CURRENT	0x06
#define STC3117_REG_OCV		0x0d
#define STC3117_REG_CC_CNF	0x0f
#define STC3117_REG_VM_CNF	0x11
#define STC3117_REG_ID		0x18
#define STC3117_REG_RAM		0x20
#define STC3117_REG_OCVTAB	0x30
#define STC3117_ID		0x16
#define STC3117_RAM_SIZE	16
#define STC3117_NTEMP		7
#define STC3117_OCV_N		16

/* Mixed mode with the alarm on, as the vendor leaves it running. */
#define STC3117_MODE_RUN	0x18

/* The cell, as the vendor's platform data and tree give it. */
struct stc3117_cell {
	int cnom_mah, rsense_mohm, rint_mohm, cc_cnf, vm_cnf;
	int alm_soc, alm_mv, relax_ma;
	/* In 0.1 %, at 60, 40, 25, 10, 0, -10 and -20 C. */
	int derating[STC3117_NTEMP];
	int ocv_mv[STC3117_OCV_N];
	uint8_t soc[STC3117_OCV_N];
};

struct stc3117_io {
	int (*read)(uint8_t reg, uint8_t *buf, unsigned n);
	int (*write)(uint8_t reg, const uint8_t *buf, unsigned n);
};

/* What the algorithm gives back after a task. */
struct stc3117_result {
	int mv, ma, decidegrees, soc, ocv_mv;	/* soc in 0.1 % */
	int avg_mv, avg_ma, avg_decidegrees, avg_soc;
	int charge_mah;
};

struct stc3117 {
	const struct stc3117_cell *cell;
	const struct stc3117_io *io;
	/* The battery data. */
	int status, voltage, current, temperature, hrsoc, ocv, conv_counter, relax_timer, cc_adj, vm_adj;
	int soc, avg_soc, avg_voltage, avg_current, avg_temperature;
	int acc_soc, acc_voltage, acc_current, acc_temperature;
	int batt_state, gg_mode, last_temperature, online;
	int current_factor, crate_factor, ropt, nropt;
	/* How the last start went: 'F' with a fresh RAM, 'P' after the
	 * chip's reset, 'R' from the RAM. */
	char start;
	uint8_t ram[STC3117_RAM_SIZE];
};

/* GasGauge_Start: < 0 without the chip. */
int stc3117_start(struct stc3117 *g, const struct stc3117_cell *cell, const struct stc3117_io *io);
/* GasGauge_Task: 1 with every result, 0 with the state of charge and
 * voltage only, -1 on an error or after the chip was reset for an
 * undervoltage or a missing battery (then voltage and state of charge
 * are filled in if the chip had them). */
int stc3117_task(struct stc3117 *g, struct stc3117_result *r);
/* GasGauge_Stop, for the watch powering off. */
int stc3117_stop(struct stc3117 *g);

uint8_t stc3117_crc8(const uint8_t *data, unsigned n);
int stc3117_interpolate(int x, int n, const int *tabx, const int *taby);
#endif
