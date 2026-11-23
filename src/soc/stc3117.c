/* ST's STC3117 gas gauge algorithm, ported from the vendor kernel's
 * stc3117_battery.c (GG_VERSION 2.03, mixed mode, TEMPCOMP_SOC, OG2
 * correction, BATD_UC8). The chip counts charge and follows the cell's
 * voltage itself; the host starts it with the cell's numbers and OCV
 * table, keeps its state in the chip's RAM with a CRC so it survives a
 * restart of the host, and corrects the state of charge every few
 * seconds. Names follow the vendor's so the two can be compared. */
#include <string.h>
#include "stc3117.h"

#define CTRL_SOFTPOR		0x11
#define REG_TEMPERATURE_N	15
#define REG_ALARM_SOC		0x13
#define REG_ALARM_VOLTAGE	0x14
#define REG_CURRENT_THRES	0x15
#define REG_CMONIT_COUNT	0x16
#define REG_CC_ADJ		0x1b
#define MODE_FORCE_CC		0x20

#define M_RST			0x1800
#define M_RUN			0x0010
#define M_GGVM			0x0400
#define M_BATFAIL		0x0800
#define M_UVLOD			0x8000

#define BATT_CHARGING		3
#define BATT_ENDCHARG		2
#define BATT_FULCHARG		1
#define BATT_IDLE		0
#define BATT_DISCHARG		(-1)
#define BATT_LOWBATT		(-2)

#define RAM_TSTWORD		0x53a9
#define GG_INIT			'I'
#define GG_RUNNING		'R'
#define GG_POWERDN		'D'
#define VM_MODE			1
#define CC_MODE			0
#define VCOUNT			0

#define VOLTAGE_FACTOR		9011
#define BATT_CHG_VOLTAGE	4350
#define BATT_MIN_VOLTAGE	3300
#define MAX_HRSOC		51200
#define MAX_SOC			1000
#define CHG_MIN_CURRENT		200
#define CHG_END_CURRENT		20
#define APP_MIN_CURRENT		(-5)
#define APP_MIN_VOLTAGE		3400
#define AVGFILTER		4
#define DELTA_TEMP		30

/* The RAM: test word, state of charge in 1/512 %, CC and VM gains, the
 * percentage for a trace and the algorithm's state, CRC in the last byte. */
#define RAM_TST			0
#define RAM_HRSOC		2
#define RAM_CC_CNF		4
#define RAM_VM_CNF		6
#define RAM_SOC			8
#define RAM_STATUS		9

static const int temp_table[STC3117_NTEMP] = { 60, 40, 25, 10, 0, -10, -20 };
static const int vm_temp_table[STC3117_NTEMP] = { 85, 90, 100, 160, 320, 440, 840 };

static uint16_t ram16(const struct stc3117 *g, unsigned at) { return (uint16_t)(g->ram[at] | g->ram[at + 1] << 8); }

static void set_ram16(struct stc3117 *g, unsigned at, int v)
{
	g->ram[at] = (uint8_t)v;
	g->ram[at + 1] = (uint8_t)(v >> 8);
}

static int read_byte(struct stc3117 *g, uint8_t reg)
{
	uint8_t v;
	return g->io->read(reg, &v, 1) ? -1 : v;
}

static int write_byte(struct stc3117 *g, uint8_t reg, int v)
{
	uint8_t b = (uint8_t)v;
	return g->io->write(reg, &b, 1);
}

static int read_word(struct stc3117 *g, uint8_t reg)
{
	uint8_t b[2];
	return g->io->read(reg, b, 2) ? -1 : b[0] | b[1] << 8;
}

static int write_word(struct stc3117 *g, uint8_t reg, int v)
{
	uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
	return g->io->write(reg, b, 2);
}

uint8_t stc3117_crc8(const uint8_t *data, unsigned n)
{
	unsigned crc = 0;
	for (unsigned i = 0; i < n; i++) {
		crc ^= data[i];
		for (int j = 0; j < 8; j++) {
			crc <<= 1;
			if (crc & 0x100)
				crc ^= 7;
		}
	}
	return (uint8_t)crc;
}

int stc3117_interpolate(int x, int n, const int *tabx, const int *taby)
{
	if (x >= tabx[0])
		return taby[0];
	if (x <= tabx[n - 1])
		return taby[n - 1];
	int index;
	for (index = 1; index < n; index++)
		if (x > tabx[index])
			break;
	if (tabx[index - 1] == tabx[index])
		return taby[index];
	int y = (taby[index - 1] - taby[index]) * (x - tabx[index]) * 2 / (tabx[index - 1] - tabx[index]);
	return (y + 1) / 2 + taby[index];
}

static int conv(int value, int factor)
{
	int v = (int)(((int64_t)value * factor) >> 11);
	return (v + 1) / 2;
}

static int status(struct stc3117 *g)
{
	if (read_byte(g, STC3117_REG_ID) != STC3117_ID)
		return -1;
	return read_word(g, STC3117_REG_MODE);
}

static void set_param(struct stc3117 *g)
{
	const struct stc3117_cell *c = g->cell;
	write_byte(g, STC3117_REG_MODE, 0x01);
	for (int i = 0; i < STC3117_OCV_N; i++)
		if (c->ocv_mv[i])
			write_word(g, (uint8_t)(STC3117_REG_OCVTAB + i * 2), c->ocv_mv[i] * 100 / 55);
	/* A word to each byte, as the vendor does: the last one also clears
	 * the byte after the table. */
	for (int i = 0; i < STC3117_OCV_N; i++)
		write_word(g, (uint8_t)(STC3117_REG_OCVTAB + STC3117_OCV_N * 2 + i), c->soc[i]);
	if (c->alm_soc)
		write_byte(g, REG_ALARM_SOC, c->alm_soc * 2);
	if (c->alm_mv)
		write_byte(g, REG_ALARM_VOLTAGE, (c->alm_mv << 9) / VOLTAGE_FACTOR);
	if (c->relax_ma)
		write_byte(g, REG_CURRENT_THRES, c->relax_ma);
	if (ram16(g, RAM_CC_CNF))
		write_word(g, STC3117_REG_CC_CNF, ram16(g, RAM_CC_CNF));
	if (ram16(g, RAM_VM_CNF))
		write_word(g, STC3117_REG_VM_CNF, ram16(g, RAM_VM_CNF));
	write_byte(g, STC3117_REG_CTRL, 0x03);
	write_byte(g, STC3117_REG_MODE, STC3117_MODE_RUN);
}

/* A fresh start: the OCV the chip measured, less the drop the current
 * makes across the cell's resistance, starts the state of charge. */
static int startup(struct stc3117 *g)
{
	if (status(g) < 0)
		return -1;
	int ocv = read_word(g, STC3117_REG_OCV);
	set_param(g);
	int curr = read_word(g, STC3117_REG_CURRENT) & 0x3fff;
	if (curr >= 0x2000)
		curr -= 0x4000;
	if (!g->cell->rsense_mohm)
		return -1;
	ocv = ocv - g->cell->rint_mohm * curr * 588 / g->cell->rsense_mohm / 55000;
	write_word(g, STC3117_REG_OCV, ocv);
	return 0;
}

static int restore(struct stc3117 *g)
{
	if (status(g) < 0)
		return -1;
	read_word(g, STC3117_REG_OCV);
	set_param(g);
	if (g->ram[RAM_STATUS] == GG_RUNNING && g->ram[RAM_SOC])
		write_word(g, STC3117_REG_SOC, ram16(g, RAM_HRSOC));
	return 0;
}

static void set_soc(struct stc3117 *g, int soc) { write_word(g, STC3117_REG_SOC, soc); }

static void save_vm_cnf(struct stc3117 *g)
{
	int mode = g->status & 0xff;
	write_byte(g, STC3117_REG_MODE, mode & ~0x10);
	read_byte(g, STC3117_REG_ID);
	write_word(g, STC3117_REG_VM_CNF, ram16(g, RAM_VM_CNF));
	write_byte(g, STC3117_REG_MODE, STC3117_MODE_RUN);
	write_byte(g, STC3117_REG_MODE, g->gg_mode == CC_MODE ? 0x38 : 0x58);
}

static int read_battery_data(struct stc3117 *g)
{
	int res = status(g);
	if (res < 0)
		return res;
	g->status = res;
	g->gg_mode = res & M_GGVM ? VM_MODE : CC_MODE;

	uint8_t d[REG_TEMPERATURE_N];
	if (g->io->read(0, d, sizeof d))
		return -1;
	g->hrsoc = d[3] << 8 | d[2];
	g->conv_counter = d[5] << 8 | d[4];
	int v = (d[7] << 8 | d[6]) & 0x3fff;
	if (v >= 0x2000)
		v -= 0x4000;
	g->current = conv(v, g->current_factor);
	v = (d[9] << 8 | d[8]) & 0x0fff;
	if (v >= 0x0800)
		v -= 0x1000;
	g->voltage = conv(v, VOLTAGE_FACTOR);
	v = d[10];
	if (v >= 0x80)
		v -= 0x100;
	g->temperature = v * 10;
	v = d[12] << 8 | d[11];
	if (v >= 0x8000)
		v -= 0x10000;
	g->avg_current = conv(v, g->current_factor) / 4;
	v = (d[14] << 8 | d[13]) & 0x3fff;
	if (v >= 0x2000)
		v -= 0x4000;
	g->ocv = (conv(v, VOLTAGE_FACTOR) + 2) / 4;

	uint8_t adj[4];
	if (g->io->read(REG_CC_ADJ, adj, sizeof adj))
		return -1;
	g->cc_adj = (int16_t)(adj[1] << 8 | adj[0]);
	g->vm_adj = (int16_t)(adj[3] << 8 | adj[2]);
	int relax = read_byte(g, REG_CMONIT_COUNT);
	if (relax < 0)
		return -1;
	g->relax_timer = relax;
	return 0;
}

static void update_ram_crc(struct stc3117 *g) { g->ram[STC3117_RAM_SIZE - 1] = stc3117_crc8(g->ram, STC3117_RAM_SIZE - 1); }

static void init_ram(struct stc3117 *g)
{
	memset(g->ram, 0, sizeof g->ram);
	set_ram16(g, RAM_TST, RAM_TSTWORD);
	set_ram16(g, RAM_CC_CNF, g->cell->cc_cnf);
	set_ram16(g, RAM_VM_CNF, g->cell->vm_cnf);
	update_ram_crc(g);
}

static bool ram_valid(const struct stc3117 *g)
{
	return ram16(g, RAM_TST) == RAM_TSTWORD && stc3117_crc8(g->ram, STC3117_RAM_SIZE) == 0;
}

static int read_ram(struct stc3117 *g) { return g->io->read(STC3117_REG_RAM, g->ram, STC3117_RAM_SIZE); }
static int write_ram(struct stc3117 *g) { return g->io->write(STC3117_REG_RAM, g->ram, STC3117_RAM_SIZE); }

static int compensate_soc(struct stc3117 *g, int value, int temp)
{
	int r = stc3117_interpolate(temp / 10, STC3117_NTEMP, temp_table, g->cell->derating);
	if (MAX_SOC == r)
		return 0;
	int v = (int)((int64_t)(value - r) * MAX_SOC * 2 / (MAX_SOC - r));
	v = (v + 1) / 2;
	return v < 0 ? 0 : v > MAX_SOC ? MAX_SOC : v;
}

static void mm_fsm(struct stc3117 *g)
{
	switch (g->batt_state) {
	case BATT_CHARGING:
		if (g->avg_current < CHG_MIN_CURRENT)
			g->batt_state = BATT_ENDCHARG;
		break;
	case BATT_ENDCHARG:
		if (g->current > CHG_MIN_CURRENT)
			g->batt_state = BATT_CHARGING;
		else if (g->avg_current < CHG_END_CURRENT)
			g->batt_state = BATT_IDLE;
		else if (g->current > CHG_END_CURRENT && g->voltage > BATT_CHG_VOLTAGE)
			g->batt_state = BATT_FULCHARG;
		break;
	case BATT_FULCHARG:
		if (g->current > CHG_MIN_CURRENT) {
			g->batt_state = BATT_CHARGING;
		} else if (g->avg_current < CHG_END_CURRENT) {
			if (g->avg_voltage > BATT_CHG_VOLTAGE) {
				set_soc(g, MAX_HRSOC);
				g->soc = MAX_SOC;
			}
			g->batt_state = BATT_IDLE;
		}
		break;
	case BATT_IDLE:
		if (g->current > CHG_END_CURRENT)
			g->batt_state = BATT_CHARGING;
		else if (g->current < APP_MIN_CURRENT)
			g->batt_state = BATT_DISCHARG;
		break;
	case BATT_DISCHARG:
		if (g->current > APP_MIN_CURRENT)
			g->batt_state = BATT_IDLE;
		else if (g->avg_voltage < BATT_MIN_VOLTAGE)
			g->batt_state = BATT_LOWBATT;
		break;
	case BATT_LOWBATT:
		if (g->avg_voltage > BATT_MIN_VOLTAGE + 50)
			g->batt_state = BATT_IDLE;
		break;
	default:
		g->batt_state = BATT_IDLE;
	}
}

static void compensate_vm(struct stc3117 *g, int temp)
{
	int r = stc3117_interpolate(temp / 10, STC3117_NTEMP, temp_table, vm_temp_table);
	set_ram16(g, RAM_VM_CNF, g->cell->vm_cnf * r / 100);
	save_vm_cnf(g);
}

/* OG2: the coulomb counter's and the voltage model's adjustments are
 * mixed by a gain that falls with the current, and the result written
 * back once they drift apart. */
static void soc_correction(struct stc3117 *g)
{
	const int current_th = g->cell->cnom_mah / 10, gain = 10, a_var3 = 500, var1max = 64, var2max = 128, var4max = 128;
	int var3 = g->soc > 800 ? 600 : g->soc > 500 ? 400 : g->soc > 250 ? 200 : g->soc > 100 ? 300 : 400;
	int64_t v1 = (int64_t)256 * g->avg_current * a_var3 / var3 / current_th;
	int var1 = (int)(32768 * gain / (256 + v1 * v1 / 256) / 10);
	var1 = (var1 + 1) / 2;
	if (var1 == 0)
		var1 = 1;
	if (var1 >= var1max)
		var1 = var1max - 1;

	int var4 = g->cc_adj - g->vm_adj, socopt;
	if (g->gg_mode == CC_MODE)
		socopt = g->hrsoc + var1 * var4 / 64;
	else
		socopt = g->hrsoc - g->cc_adj + var1 * var4 / 64;

	int var2 = g->nropt;
	if (g->avg_current < -current_th || g->avg_current > current_th) {
		if (var2 < var2max)
			var2++;
		if (g->avg_current && var2)
			g->ropt = g->ropt + (1000 * (g->voltage - g->ocv) / g->avg_current - g->ropt / var2);
		g->nropt = var2;
	}

	if (socopt <= 0)
		socopt = 0;
	if (socopt >= MAX_HRSOC)
		socopt = MAX_HRSOC;
	g->soc = (socopt * 10 + 256) / 512;
	if (var4 < -var4max || var4 >= var4max)
		set_soc(g, socopt);
}

int stc3117_start(struct stc3117 *g, const struct stc3117_cell *cell, const struct stc3117_io *io)
{
	memset(g, 0, sizeof *g);
	g->cell = cell;
	g->io = io;
	g->online = 1;
	g->current_factor = 24084 / (cell->rsense_mohm ? cell->rsense_mohm : 10);
	g->crate_factor = 36 * cell->cnom_mah;

	int res;
	if (read_ram(g) || !ram_valid(g)) {
		init_ram(g);
		res = startup(g);
	} else if (status(g) & M_RST) {
		res = startup(g);
	} else {
		res = restore(g);
	}
	g->ram[RAM_STATUS] = GG_INIT;
	update_ram_crc(g);
	write_ram(g);
	g->batt_state = BATT_IDLE;
	return res;
}

static void reset(struct stc3117 *g)
{
	set_ram16(g, RAM_TST, 0);
	g->ram[RAM_STATUS] = 0;
	write_ram(g);
	write_byte(g, STC3117_REG_CTRL, CTRL_SOFTPOR);
}

int stc3117_stop(struct stc3117 *g)
{
	read_ram(g);
	g->ram[RAM_STATUS] = GG_POWERDN;
	update_ram_crc(g);
	write_ram(g);
	write_byte(g, STC3117_REG_CTRL, 0x01);
	return write_byte(g, STC3117_REG_MODE, 0x19) ? -1 : 0;
}

int stc3117_task(struct stc3117 *g, struct stc3117_result *r)
{
	if (read_battery_data(g))
		return -1;

	read_ram(g);
	if (!ram_valid(g)) {
		init_ram(g);
		g->ram[RAM_STATUS] = GG_INIT;
	}
	if (g->status & M_BATFAIL)
		g->online = 0;
	if (g->status & (M_BATFAIL | M_UVLOD)) {
		if (g->conv_counter > 0) {
			r->mv = g->voltage;
			r->soc = (g->hrsoc * 10 + 256) / 512;
		}
		reset(g);
		return -1;
	}
	if (!(g->status & M_RUN)) {
		restore(g);
		g->ram[RAM_STATUS] = GG_INIT;
	}

	g->soc = (g->hrsoc * 10 + 256) / 512;

	if (g->ram[RAM_STATUS] == GG_INIT && g->conv_counter > VCOUNT) {
		compensate_vm(g, g->temperature);
		g->last_temperature = g->temperature;
		g->avg_voltage = g->voltage;
		g->avg_current = g->current;
		g->avg_temperature = g->temperature;
		g->avg_soc = compensate_soc(g, g->soc, g->temperature);
		g->acc_voltage = g->avg_voltage * AVGFILTER;
		g->acc_current = g->avg_current * AVGFILTER;
		g->acc_temperature = g->avg_temperature * AVGFILTER;
		g->acc_soc = g->avg_soc * AVGFILTER;
		g->ram[RAM_STATUS] = GG_RUNNING;
	}

	int ret;
	if (g->ram[RAM_STATUS] != GG_RUNNING) {
		*r = (struct stc3117_result){ .soc = compensate_soc(g, g->soc, 250), .mv = g->voltage, .ocv_mv = g->ocv,
					      .decidegrees = 250 };
		ret = 0;
	} else {
		if (!(g->status & M_BATFAIL))
			g->online = 1;
		soc_correction(g);
		g->soc = compensate_soc(g, g->soc, g->temperature);

		/* Early empty compensation. */
		int v = g->avg_voltage;
		if (g->voltage < v)
			v = g->voltage;
		if (v < APP_MIN_VOLTAGE + 200 && v > APP_MIN_VOLTAGE - 500)
			g->soc = v < APP_MIN_VOLTAGE ? 0 : g->soc * (v - APP_MIN_VOLTAGE) / 200;

		g->acc_voltage += g->voltage - g->avg_voltage;
		g->acc_current += g->current - g->avg_current;
		g->acc_temperature += g->temperature - g->avg_temperature;
		g->acc_soc += g->soc - g->avg_soc;
		g->avg_voltage = (g->acc_voltage + AVGFILTER / 2) / AVGFILTER;
		g->avg_temperature = (g->acc_temperature + AVGFILTER / 2) / AVGFILTER;
		g->avg_soc = (g->acc_soc + AVGFILTER / 2) / AVGFILTER;

		mm_fsm(g);

		/* Late full and late empty compensation. */
		if (g->avg_current > 0 && g->soc >= 990 && g->soc < 995 && g->avg_current > 100) {
			g->soc = 990;
			set_soc(g, 99 * 512);
		}
		if (g->avg_current < 0 && g->soc >= 15 && g->soc < 20 && g->voltage > APP_MIN_VOLTAGE + 50) {
			g->soc = 20;
			set_soc(g, 2 * 512);
		}

		*r = (struct stc3117_result){ .mv = g->voltage, .ma = g->current, .decidegrees = g->temperature, .soc = g->soc,
					      .ocv_mv = g->ocv, .avg_mv = g->avg_voltage, .avg_ma = g->avg_current,
					      .avg_decidegrees = g->avg_temperature, .avg_soc = g->avg_soc,
					      .charge_mah = g->cell->cnom_mah * g->avg_soc / MAX_SOC };
		ret = 1;
	}

	set_ram16(g, RAM_HRSOC, g->hrsoc);
	g->ram[RAM_SOC] = (uint8_t)((r->soc + 5) / 10);
	update_ram_crc(g);
	write_ram(g);
	return ret;
}
