/* Fuel gauge: the state of charge as the vendor's battery monitor driver
 * works it out. The PMIC's battery monitor measures the open circuit
 * voltage on its own while the load is low and counts the charge through
 * the sense resistor since then. The charge left is the cell's capacity
 * at that voltage (from the HB442528EBC tables) minus the counted charge
 * and minus the charge that cannot be used before the voltage under the
 * average load drops to the cutoff. Near empty and while charging the
 * result is corrected with the voltage under load, and the reported value
 * moves by at most 1 % per step. The reported value is kept in a PMIC
 * register, so it survives a reset. */
#include "asteroid.h"

#define BMS_CC_DATA_CTL		0x004042
#define BMS_CC_CLEAR_CTL	0x004043
#define BMS_CC_DATA0		0x00408a
#define BMS_OCV_FOR_SOC		0x004090
#define BMS_VSENSE_AVG		0x004098
#define BMS_SOC_STORAGE		0x0040b0
#define BMS_IAVG_STORAGE	0x0040b1
#define HOLD_OREG_DATA		BIT(0)
#define CLEAR_CC		BIT(7)

#define CHAN_DIE_TEMP		8
#define CHAN_REF_625MV		9
#define CHAN_REF_1250MV		10
#define CHAN_GND_REF		14
#define CHAN_VDD_VADC		15
#define CHAN_VBAT_SNS		6
#define CHAN_BATT_THERM		0x30
#define CHAN_BATT_ID		0x31

#define RSENSE_UOHM		9700
#define V_CUTOFF_UV		3200000
#define V_MAX_UV		4350000
#define CHG_TERM_UA		20000
#define FLAT_OCV_UV		3800000
#define LOW_VOLTAGE_UV		3350000
#define ADJUST_SOC_LOW		15
#define HOLD_SOC_EST		3
#define LOW_OCV_CORR_UV		100
#define HIGH_OCV_CORR_UV	250
#define SHUTDOWN_SOC_LIMIT	50
#define CALC_MS			20000
#define LOW_SOC_CALC_MS		5000
#define LOW_SOC			15
#define LOW_VOLTAGE_CALC_MS	1000
#define MIN_IAVG_MA		60
#define IAVG_STEP_MA		10
#define IAVG_SAMPLES		16
#define CUTOFF_DELTA_UV		50000
#define OCV_VBAT_MAX_DELTA_UV	200000
#define CUTOFF_CONFIRM		4
#define PROTECT_UV		3250000
#define PROTECT_COUNT		10
#define HOLD_DROP		6
#define CHARGER_GONE_HOLD_S	10
#define NONE			(-1000)

static const int8_t lut_temp[5] = { -20, 0, 25, 40, 60 };
static const uint8_t lut_pc[31] = { 100, 95, 90, 85, 80, 75, 70, 65, 60, 55, 50, 45, 40, 35, 30, 25,
				    20, 16, 13, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
static const uint16_t fcc_mah[5] = { 322, 321, 322, 321, 320 };
static const uint16_t ocv_mv[31][5] = {
	{ 4351, 4348, 4344, 4340, 4334 }, { 4253, 4275, 4279, 4277, 4272 }, { 4182, 4215, 4222, 4220, 4216 },
	{ 4121, 4158, 4166, 4164, 4160 }, { 4067, 4107, 4112, 4112, 4108 }, { 4000, 4056, 4062, 4061, 4058 },
	{ 3942, 4000, 4014, 4014, 4011 }, { 3900, 3948, 3970, 3970, 3967 }, { 3864, 3905, 3922, 3924, 3922 },
	{ 3836, 3868, 3878, 3878, 3877 }, { 3817, 3838, 3846, 3846, 3845 }, { 3802, 3814, 3822, 3822, 3821 },
	{ 3788, 3795, 3802, 3802, 3800 }, { 3775, 3782, 3784, 3784, 3782 }, { 3760, 3770, 3769, 3764, 3754 },
	{ 3744, 3756, 3750, 3742, 3728 }, { 3726, 3735, 3728, 3719, 3706 }, { 3708, 3712, 3704, 3694, 3682 },
	{ 3691, 3694, 3685, 3678, 3667 }, { 3676, 3686, 3682, 3674, 3664 }, { 3668, 3682, 3679, 3672, 3661 },
	{ 3658, 3678, 3676, 3670, 3658 }, { 3648, 3672, 3672, 3664, 3650 }, { 3632, 3663, 3660, 3648, 3628 },
	{ 3610, 3642, 3627, 3615, 3594 }, { 3579, 3602, 3586, 3573, 3558 }, { 3536, 3550, 3539, 3528, 3514 },
	{ 3475, 3488, 3484, 3472, 3454 }, { 3392, 3410, 3406, 3389, 3364 }, { 3274, 3298, 3283, 3260, 3229 },
	{ 3000, 3000, 3000, 3000, 3000 },
};

/* Two cell makers, told apart by the ID resistor in the pack; they share
 * the voltage table but not the internal resistance. */
struct cell {
	const char *name;
	int id_kohm, rbatt_mohm;
	uint16_t rbatt_sf[31][5];
};

static const struct cell cells[] = {
	{ "SCUD", 10, 496, {
		{ 1926, 401, 100, 65, 47 }, { 1916, 401, 100, 65, 47 }, { 1767, 405, 102, 67, 48 },
		{ 1681, 407, 105, 69, 49 }, { 1650, 414, 110, 72, 52 }, { 1598, 422, 116, 76, 54 },
		{ 1570, 415, 124, 82, 58 }, { 1567, 396, 133, 90, 63 }, { 1568, 383, 122, 85, 61 },
		{ 1585, 374, 103, 71, 52 }, { 1616, 376, 99, 66, 49 }, { 1658, 383, 101, 68, 51 },
		{ 1711, 395, 104, 73, 54 }, { 1768, 415, 108, 78, 58 }, { 1855, 441, 110, 73, 50 },
		{ 1954, 470, 105, 67, 48 }, { 2070, 501, 105, 68, 49 }, { 2070, 503, 105, 67, 48 },
		{ 2171, 529, 103, 65, 47 }, { 2276, 565, 107, 68, 50 }, { 2336, 589, 111, 72, 51 },
		{ 2474, 617, 116, 75, 53 }, { 2673, 649, 124, 77, 54 }, { 2990, 690, 126, 76, 54 },
		{ 3396, 719, 126, 78, 59 }, { 3953, 764, 136, 89, 72 }, { 4767, 834, 157, 108, 84 },
		{ 6044, 932, 179, 117, 78 }, { 8223, 1079, 191, 120, 87 }, { 14102, 1430, 241, 166, 142 },
		{ 28204, 2860, 482, 332, 284 },
	} },
	{ "XWD", 470, 487, {
		{ 2059, 415, 100, 64, 46 }, { 2053, 415, 100, 64, 46 }, { 1925, 416, 101, 65, 46 },
		{ 1843, 416, 104, 67, 47 }, { 1808, 421, 107, 69, 49 }, { 1755, 431, 113, 73, 51 },
		{ 1730, 422, 120, 77, 54 }, { 1725, 408, 128, 83, 58 }, { 1722, 400, 127, 86, 60 },
		{ 1732, 392, 111, 77, 54 }, { 1762, 390, 101, 67, 48 }, { 1810, 394, 101, 68, 49 },
		{ 1874, 405, 105, 71, 53 }, { 1958, 425, 108, 76, 56 }, { 2067, 455, 112, 72, 52 },
		{ 2198, 492, 109, 67, 48 }, { 2345, 527, 107, 68, 49 }, { 2431, 556, 108, 67, 48 },
		{ 2347, 575, 105, 65, 47 }, { 2534, 622, 111, 69, 50 }, { 2752, 653, 115, 72, 51 },
		{ 3012, 689, 120, 75, 52 }, { 3324, 730, 127, 77, 53 }, { 3739, 767, 129, 77, 53 },
		{ 4292, 808, 130, 78, 58 }, { 5049, 867, 141, 89, 70 }, { 6129, 957, 162, 109, 86 },
		{ 7924, 1086, 197, 127, 86 }, { 11469, 1426, 233, 137, 92 }, { 22938, 2852, 344, 206, 128 },
		{ 45877, 5703, 688, 411, 255 },
	} },
};

/* Battery thermistor: 0.1 C against mV at the ADC input, with a 68 kOhm
 * pull-up to 1.8 V. */
static const struct { int16_t t; int16_t mv; } therm[] = {
	{ -250, 1566 }, { -200, 1513 }, { -150, 1453 }, { -100, 1389 }, { -50, 1320 }, { 0, 1249 }, { 50, 1177 },
	{ 100, 1107 }, { 150, 1039 }, { 200, 974 }, { 250, 914 }, { 300, 860 }, { 350, 811 }, { 400, 767 },
	{ 450, 728 }, { 500, 694 }, { 550, 664 }, { 600, 638 }, { 650, 615 }, { 700, 595 },
};

static const struct cell *cell = &cells[0];
static int temp, die_mc, v0625_uv, v1250_uv;
static int last_ocv_uv, last_ocv_temp, ocv_reading_at_100 = -1, prev_ocv_raw = -1;
static int64_t sw_cc_uah;
static int last_cc_uah;
static bool have_last_cc;
static uint32_t tm_sec, last_recalc_sec, last_change_sec, charge_start_sec, charger_gone_sec, poll_sec;
static int iavg_samples[IAVG_SAMPLES], iavg_index, iavg_num;
static bool first_uuc = true, first_soc = true;
static int prev_pc_unusable = NONE, prev_uuc_iavg_ma;
static int shutdown_soc, shutdown_iavg_ma = MIN_IAVG_MA;
static bool shutdown_soc_invalid;
static int calculated_soc = NONE, last_soc = NONE, last_soc_est = NONE, held_soc = NONE;
static int soc_at_cv = NONE, prev_chg_soc = NONE, ibat_at_cv_ua, prev_terminal_uv, system_load_count;
static int charging_adjusted_ocv = NONE, catch_up_sec;
static bool done_charging, last_soc_invalid, last_soc_unbound, was_charging, low_voltage;
static int cutoff_count, bad_voltage_count;
static bool at_cutoff, fake_low, charger_gone_hold, usb_was;
static int status = BAT_DISCHARGING;

struct params {
	int delta_s, fcc_uah, ocv_charge_uah, cc_uah, rbatt_mohm, iavg_ua, uuc_uah;
};

static uint32_t now_sec(void)
{
	return (uint32_t)(timer_count() / timer_hz());
}

static int delta_time(uint32_t *stamp)
{
	uint32_t now = now_sec();
	int d = (int)(now - *stamp);
	*stamp = now;
	return d;
}

static int iabs(int v)
{
	return v < 0 ? -v : v;
}

static int bound_soc(int soc)
{
	return soc < 0 ? 0 : soc > 100 ? 100 : soc;
}

static int lerp(int y0, int x0, int y1, int x1, int x)
{
	if (y0 == y1 || x == x0)
		return y0;
	if (x1 == x0 || x == x1)
		return y1;
	return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
}

/* The two table columns around a temperature, and the temperature
 * clamped to the table. */
static int temp_cols(int t, int *j)
{
	if (t < lut_temp[0] * 10)
		t = lut_temp[0] * 10;
	if (t > lut_temp[4] * 10)
		t = lut_temp[4] * 10;
	for (*j = 0; *j < 4 && t > lut_temp[*j] * 10; (*j)++)
		;
	return t;
}

static void pc_rows(int pc, int *r1, int *r2)
{
	*r1 = *r2 = 0;
	if (pc > lut_pc[0])
		return;
	*r1 = *r2 = 30;
	if (pc < lut_pc[30])
		return;
	for (int i = 0; i < 31; i++) {
		if (pc == lut_pc[i]) {
			*r1 = *r2 = i;
			return;
		}
		if (pc > lut_pc[i]) {
			*r1 = i - 1;
			*r2 = i;
			return;
		}
	}
}

/* A value of a percent x temperature table, interpolated in both. */
static int lut2(const uint16_t lut[31][5], int t, int pc)
{
	int r1, r2, j;
	pc_rows(pc, &r1, &r2);
	t = temp_cols(t, &j);
	if (t == lut_temp[j] * 10)
		return lerp(lut[r1][j], lut_pc[r1], lut[r2][j], lut_pc[r2], pc);
	int a = lerp(lut[r1][j - 1], lut_temp[j - 1] * 10, lut[r1][j], lut_temp[j] * 10, t);
	int b = lerp(lut[r2][j - 1], lut_temp[j - 1] * 10, lut[r2][j], lut_temp[j] * 10, t);
	return lerp(a, lut_pc[r1], b, lut_pc[r2], pc);
}

static int interpolate_ocv(int t, int pc)
{
	return lut2(ocv_mv, t, pc);
}

static int col_pc(int j, int mv, bool *found)
{
	for (int i = 0; i < 30; i++) {
		int a = ocv_mv[i][j], b = ocv_mv[i + 1][j];
		if ((a >= b && a >= mv && mv >= b) || (a <= b && a <= mv && mv <= b)) {
			*found = true;
			return lerp(lut_pc[i], a, lut_pc[i + 1], b, mv);
		}
	}
	*found = false;
	return 0;
}

/* Percent of capacity at an open circuit voltage. */
static int interpolate_pc(int t, int mv)
{
	int j;
	t = temp_cols(t, &j);
	if (t == lut_temp[j] * 10) {
		if (mv >= ocv_mv[0][j])
			return lut_pc[0];
		if (mv <= ocv_mv[30][j])
			return lut_pc[30];
		for (int i = 0; i < 31; i++)
			if (mv >= ocv_mv[i][j])
				return mv == ocv_mv[i][j] ? lut_pc[i] : lerp(lut_pc[i], ocv_mv[i][j], lut_pc[i - 1], ocv_mv[i - 1][j], mv);
	}
	if (mv >= ocv_mv[0][j])
		return lut_pc[0];
	if (mv <= ocv_mv[30][j - 1])
		return lut_pc[30];
	bool f0, f1;
	int p0 = col_pc(j - 1, mv, &f0), p1 = col_pc(j, mv, &f1);
	if (f0 && f1)
		return lerp(p0, lut_temp[j - 1] * 10, p1, lut_temp[j] * 10, t);
	return f1 ? p1 : f0 ? p0 : 100;
}

static int calculate_pc(int uv, int t)
{
	return interpolate_pc(t, uv / 1000);
}

static int calculate_fcc(int t)
{
	int j;
	t = temp_cols(t, &j);
	if (t == lut_temp[j] * 10)
		return fcc_mah[j] * 1000;
	return lerp(fcc_mah[j - 1], lut_temp[j - 1] * 10, fcc_mah[j], lut_temp[j] * 10, t) * 1000;
}

static int get_rbatt(int soc, int t)
{
	return cell->rbatt_mohm * lut2(cell->rbatt_sf, t, soc) / 100;
}

/* ADC readings. The voltage ADC uses the 0.625 V and 1.25 V references
 * for absolute channels and ground and its supply for ratiometric ones;
 * a code of 0x6000 is 0 V, 97.656 uV per step. */
static int code_uv(int code)
{
	return code <= 0x6000 ? 0 : (code - 0x6000) * 97656 / 1000;
}

static void vadc_calibrate(void)
{
	int c625 = pmic_vadc_raw(CHAN_REF_625MV, 0), c1250 = pmic_vadc_raw(CHAN_REF_1250MV, 0);
	int code = pmic_vadc_raw(CHAN_DIE_TEMP, 0);
	v0625_uv = code_uv(c625);
	v1250_uv = code_uv(c1250);
	if (c1250 > c625 && code > 0)
		die_mc = (int)(((int64_t)(code - c625) * 625000 / (c1250 - c625) + 625000) / 2 - 273160);
}

static int ratiometric_mv(uint8_t chan)
{
	int gnd = pmic_vadc_raw(CHAN_GND_REF, 0), vdd = pmic_vadc_raw(CHAN_VDD_VADC, 0);
	int code = pmic_vadc_raw(chan, 2);
	if (vdd <= gnd || code < 0)
		return -1;
	return (code - gnd) * 1800 / (vdd - gnd);
}

static int battery_temp(void)
{
	int mv = ratiometric_mv(CHAN_BATT_THERM);
	if (mv < 0)
		return 250;
	unsigned i = 0;
	while (i < ARRAY_SIZE(therm) && therm[i].mv >= mv)
		i++;
	if (i == 0)
		return therm[0].t;
	if (i == ARRAY_SIZE(therm))
		return therm[i - 1].t;
	return (therm[i].t - therm[i - 1].t) * (mv - therm[i - 1].mv) / (therm[i].mv - therm[i - 1].mv) + therm[i - 1].t;
}

/* Battery voltage register reading to uV, corrected for the PMIC's
 * known offset; the voltage at power on also for the die temperature. */
static int vbat_raw_uv(int raw, bool pon)
{
	int64_t uv = code_uv(raw);
	if (!uv)
		return 0;
	if (v1250_uv > v0625_uv)
		uv = (625000 + (uv - v0625_uv) * 625000 / (v1250_uv - v0625_uv)) * 3;
	else
		uv *= 3;
	if (!pon)
		return (int)(uv - 3500);
	if (die_mc > 25000)
		return (int)uv;
	uv -= 6800;
	return (int)(uv * (1000000 + (int64_t)(die_mc - 25000) * 8060 / 100000) / 1000000);
}

static int vbat_uv(void)
{
	int c625 = pmic_vadc_raw(CHAN_REF_625MV, 0), c1250 = pmic_vadc_raw(CHAN_REF_1250MV, 0);
	int code = pmic_vadc_raw(CHAN_VBAT_SNS, 0);
	if (c1250 <= c625 || code < 0)
		return 0;
	return (int)(((int64_t)(code - c625) * 625000 / (c1250 - c625) + 625000) * 3 - 3500);
}

/* The current ADC's error at the die temperature while discharging. */
static int64_t iadc_comp(int64_t ua)
{
	if (ua < 0)
		return ua;
	int64_t t = (int64_t)33 * die_mc / 100;
	return ua * 1000000 / (1000000 - t);
}

static int64_t sense_uv(int64_t reading)
{
	int gain = pmic_iadc_gain();
	int64_t uv = reading * 542535 / 100000;
	return gain > 0 ? uv * 3291 / gain : uv;
}

static int battery_ua(void)
{
	uint8_t d[2];
	pmic_write(BMS_CC_DATA_CTL, HOLD_OREG_DATA);
	spmi_read(BMS_VSENSE_AVG, d, 2);
	pmic_write(BMS_CC_DATA_CTL, 0);
	return (int)iadc_comp(sense_uv((int16_t)(d[0] | d[1] << 8)) * 1000000 / RSENSE_UOHM);
}

static void reset_cc(void)
{
	pmic_write(BMS_CC_CLEAR_CTL, CLEAR_CC);
	delay_us(100);
	pmic_write(BMS_CC_CLEAR_CTL, 0);
}

static int find_ocv_for_pc(int t, int pc)
{
	int mv = interpolate_ocv(t, pc), new_pc = interpolate_pc(t, mv), step = 5, count = 0;
	int spins = 1 + (V_MAX_UV - V_CUTOFF_UV) / 50000;
	int sign = pc - new_pc < 0 ? -1 : 1;
	while (new_pc != pc && count < spins) {
		int new_sign = pc - new_pc < 0 ? -1 : 1;
		if (new_sign != sign) {
			if (step > 1)
				step = 1;
			else
				break;
		}
		sign = new_sign;
		mv += step * sign;
		new_pc = interpolate_pc(t, mv);
		count++;
	}
	return mv * 1000;
}

static int find_pc_for_soc(const struct params *p, int soc)
{
	int64_t charge = (int64_t)soc * (p->fcc_uah - p->uuc_uah) / 100 + p->cc_uah + p->uuc_uah;
	int pc = (int)((charge * 100 + (charge < 0 ? -p->fcc_uah : p->fcc_uah) / 2) / p->fcc_uah);
	return pc < 0 ? 0 : pc > 100 ? 100 : pc;
}

static int find_ocv_charge_for_soc(const struct params *p, int soc)
{
	return (int)((int64_t)soc * (p->fcc_uah - p->uuc_uah) / 100) + p->cc_uah + p->uuc_uah;
}

static void read_raw(int *ocv_raw, int64_t *cc)
{
	uint8_t o[2], c[5];
	pmic_write(BMS_CC_DATA_CTL, HOLD_OREG_DATA);
	delay_us(100);
	spmi_read(BMS_OCV_FOR_SOC, o, 2);
	spmi_read(BMS_CC_DATA0, c, 5);
	pmic_write(BMS_CC_DATA_CTL, 0);
	*ocv_raw = o[0] | o[1] << 8;
	uint64_t v = c[0] | (uint64_t)c[1] << 8 | (uint64_t)c[2] << 16 | (uint64_t)c[3] << 24 | (uint64_t)(c[4] & 0xf) << 32;
	*cc = (int64_t)(v << 28) >> 28;
}

/* Picks up a new open circuit voltage from the monitor, or makes one up:
 * at the first reading after a reset (the monitor's own is stale then)
 * from the voltage under load, and at the end of a charge the 100 %
 * voltage. */
static void read_soc_params(int *ocv_raw, int64_t *cc)
{
	read_raw(ocv_raw, cc);
	if (prev_ocv_raw < 0) {
		vadc_calibrate();
		prev_ocv_raw = *ocv_raw;
		last_ocv_uv = vbat_raw_uv(*ocv_raw, true);
		last_ocv_temp = temp;
		sw_cc_uah = 0;
		bool warm = pmic_read(0x00080a) || pmic_read(0x00080b);
		if (last_ocv_uv < 2000000 || warm) {
			last_ocv_uv = vbat_uv() + pmic_battery_ua() * get_rbatt(50, temp) / 1000;
			reset_cc();
		}
	} else if (done_charging) {
		done_charging = false;
		ocv_reading_at_100 = *ocv_raw;
		last_ocv_uv = find_ocv_for_pc(temp, 100);
		*cc = 0;
		reset_cc();
		last_ocv_temp = temp;
		sw_cc_uah = 0;
		have_last_cc = false;
	} else if (prev_ocv_raw != *ocv_raw) {
		vadc_calibrate();
		prev_ocv_raw = *ocv_raw;
		last_ocv_uv = vbat_raw_uv(*ocv_raw, false);
		last_ocv_temp = temp;
		sw_cc_uah = 0;
		have_last_cc = false;
		log_printf("bms: new ocv %d uV\n", last_ocv_uv);
	}
	if (ocv_reading_at_100 != *ocv_raw)
		ocv_reading_at_100 = -1;
}

/* Charge counted since the open circuit voltage, in uAh; the counter
 * restarts after every reading so each part gets the current gain. */
static int calculate_cc(int64_t cc)
{
	int64_t pvh = sense_uv(cc) * 56 * 100000 / (32764 * 3600) * 10;
	sw_cc_uah += iadc_comp(pvh / RSENSE_UOHM);
	reset_cc();
	return (int)sw_cc_uah;
}

/* Charge below the point where the voltage under the average load of the
 * last 16 readings reaches the cutoff, allowed to move by 1 % a minute. */
static int unusable_charge(struct params *p)
{
	int ma = p->iavg_ua / 1000;
	if (first_uuc && shutdown_iavg_ma) {
		for (int i = 0; i < IAVG_SAMPLES; i++)
			iavg_samples[i] = shutdown_iavg_ma;
		iavg_index = 0;
		iavg_num = IAVG_SAMPLES;
	}
	if (p->delta_s >= 2) {
		iavg_samples[iavg_index] = ma < MIN_IAVG_MA ? MIN_IAVG_MA : ma;
		iavg_index = (iavg_index + 1) % IAVG_SAMPLES;
		if (++iavg_num > IAVG_SAMPLES)
			iavg_num = IAVG_SAMPLES;
	}
	ma = 0;
	if (iavg_num) {
		for (int i = 0; i < iavg_num; i++)
			ma += iavg_samples[i];
		ma = (ma + iavg_num / 2) / iavg_num;
	}

	int i, rbatt = 0, delta = 0, prev_delta = 0, prev_rbatt = 0;
	for (i = 0; i <= 100; i++) {
		rbatt = get_rbatt(i, temp);
		delta = interpolate_ocv(temp, i) * 1000 - (rbatt * ma + V_CUTOFF_UV);
		if (delta > 0)
			break;
		prev_delta = delta;
		prev_rbatt = rbatt;
	}
	int uuc_rbatt = lerp(rbatt, delta, prev_rbatt, prev_delta, 0);
	int pc_unusable = calculate_pc(uuc_rbatt * ma + V_CUTOFF_UV, temp);
	int uuc = p->fcc_uah * pc_unusable / 100;
	prev_uuc_iavg_ma = ma;

	int max_change = p->delta_s / 60 > 1 ? p->delta_s / 60 : 1;
	if (first_uuc || prev_pc_unusable == NONE || iabs(prev_pc_unusable - pc_unusable) <= max_change) {
		prev_pc_unusable = pc_unusable;
		return uuc;
	}
	prev_pc_unusable += pc_unusable > prev_pc_unusable ? max_change : -max_change;
	int mv = interpolate_ocv(temp, prev_pc_unusable);
	if (mv < V_CUTOFF_UV / 1000)
		mv = V_CUTOFF_UV / 1000;
	prev_uuc_iavg_ma = (mv * 1000 - V_CUTOFF_UV) / p->rbatt_mohm;
	if (!prev_uuc_iavg_ma)
		prev_uuc_iavg_ma = 1;
	return p->fcc_uah * prev_pc_unusable / 100;
}

static void soc_params(int64_t cc, struct params *p)
{
	p->delta_s = delta_time(&tm_sec);
	p->fcc_uah = calculate_fcc(temp);
	p->ocv_charge_uah = p->fcc_uah * calculate_pc(last_ocv_uv, last_ocv_temp) / 100;
	p->cc_uah = calculate_cc(cc);
	int soc_rbatt = (p->ocv_charge_uah - p->cc_uah) * 100 / p->fcc_uah;
	p->rbatt_mohm = get_rbatt(soc_rbatt < 0 ? 0 : soc_rbatt, temp);
	if (p->delta_s < 2 || !have_last_cc)
		p->iavg_ua = battery_ua();
	else
		p->iavg_ua = (int)((int64_t)(p->cc_uah - last_cc_uah) * 3600 / p->delta_s);
	last_cc_uah = p->cc_uah;
	have_last_cc = true;
	p->uuc_uah = unusable_charge(p);
}

static int raw_soc(struct params *p)
{
	int ruc = p->ocv_charge_uah - p->cc_uah - p->uuc_uah, usable = p->fcc_uah - p->uuc_uah;
	int soc = (ruc * 100 + (ruc < 0 ? -usable : usable) / 2) / usable;
	if (first_soc && soc > -10 && soc < 0) {
		last_ocv_uv = find_ocv_for_pc(temp, find_pc_for_soc(p, 0));
		p->ocv_charge_uah = find_ocv_charge_for_soc(p, 0);
		ruc = p->ocv_charge_uah - p->cc_uah - p->uuc_uah;
		soc = (ruc * 100 + (ruc < 0 ? -usable : usable) / 2) / usable;
	}
	if (soc > 100)
		soc = 100;
	if (soc > -10 && soc < 0)
		soc = 0;
	return soc;
}

/* In constant voltage the state of charge follows the falling current
 * towards the termination current, weighted in as it nears full. */
static int charging_adjustments(const struct params *p, int soc, int vbat, int ibat)
{
	int terminal = vbat;
	if (soc_at_cv == NONE) {
		if (terminal >= V_MAX_UV - 5000) {
			soc_at_cv = prev_chg_soc = soc;
			ibat_at_cv_ua = p->iavg_ua;
		}
		prev_terminal_uv = terminal;
		system_load_count = 0;
		return soc;
	} else if (ibat > 0 && terminal < V_MAX_UV - 10000) {
		if (system_load_count > 3) {
			soc_at_cv = NONE;
			system_load_count = 0;
		} else {
			system_load_count++;
		}
		return soc;
	} else if (ibat > 0) {
		system_load_count = 0;
		prev_chg_soc = soc;
		return soc;
	}
	system_load_count = 0;
	if (terminal <= prev_terminal_uv - 10000) {
		prev_terminal_uv = terminal;
		return prev_chg_soc;
	}
	int soc_ibat = bound_soc(lerp(soc_at_cv, ibat_at_cv_ua, 100, -CHG_TERM_UA, p->iavg_ua));
	int w_ibat = bound_soc(lerp(1, soc_at_cv, 100, 100, prev_chg_soc));
	int chg_soc = bound_soc((soc_ibat * w_ibat + (100 - w_ibat) * soc + 50) / 100);
	if (chg_soc > prev_chg_soc) {
		prev_chg_soc = chg_soc;
		charging_adjusted_ocv = find_ocv_for_pc(temp, find_pc_for_soc(p, chg_soc));
	}
	prev_terminal_uv = terminal;
	return prev_chg_soc;
}

/* Below 15 % the open circuit voltage is pulled towards the one the
 * voltage under load suggests, a little each time; the last per cent is
 * only reported once that voltage is near the cutoff. */
static int adjust_soc(struct params *p, int soc)
{
	int vbat = vbat_uv(), ibat = pmic_battery_ua();
	low_voltage = vbat <= LOW_VOLTAGE_UV;
	int ocv_est = vbat + ibat * p->rbatt_mohm / 1000;
	int pc_est = calculate_pc(ocv_est, temp);
	int soc_est = bound_soc((int)(((int64_t)p->fcc_uah * pc_est - (int64_t)p->uuc_uah * 100) / (p->fcc_uah - p->uuc_uah)));
	int limit = (ibat + (ibat < 0 ? -500 : 500)) / 1000;

	if (status == BAT_CHARGING) {
		at_cutoff = fake_low = false;
		soc = charging_adjustments(p, soc, vbat, ibat);
		if (soc_at_cv != NONE || ibat < 0)
			goto out;
	}
	if (!low_voltage && (soc_est == soc || soc_est > ADJUST_SOC_LOW || soc >= 98))
		goto check_cutoff;

	if (last_soc_est == NONE)
		last_soc_est = soc;
	int n = soc + soc_est + last_soc_est;
	n = n < 1 ? 1 : n > 200 ? 200 : n;
	last_soc_est = soc_est;
	int slope = 0, pc = calculate_pc(last_ocv_uv, last_ocv_temp), pc_new;
	if (pc > 0) {
		pc_new = calculate_pc(last_ocv_uv - ++slope * 1000, last_ocv_temp);
		while (pc_new == pc) {
			slope += 10;
			pc_new = calculate_pc(last_ocv_uv - slope * 1000, last_ocv_temp);
		}
	} else {
		pc = slope = 1;
		pc_new = 0;
	}
	int delta = (int)((int64_t)(soc - soc_est) * slope * 1000 / (n * (pc - pc_new)));
	if (iabs(delta) > limit)
		delta = delta > 0 ? limit : -limit;
	if (low_voltage) {
		if (delta < 0)
			delta = 0;
	} else {
		int corr = last_ocv_uv > FLAT_OCV_UV ? HIGH_OCV_CORR_UV : LOW_OCV_CORR_UV;
		if (iabs(delta) > corr)
			delta = delta > 0 ? corr : -corr;
	}
	last_ocv_uv -= delta;
	if (last_ocv_uv >= V_MAX_UV)
		last_ocv_uv = V_MAX_UV;
	pc_new = calculate_pc(last_ocv_uv, last_ocv_temp);
	int soc_new = (p->fcc_uah * pc_new / 100 - p->cc_uah - p->uuc_uah) * 100 / (p->fcc_uah - p->uuc_uah);
	if (soc_new == 0 && soc_est >= HOLD_SOC_EST)
		soc_new = 1;
	soc = soc_new;

check_cutoff:;
	int est = ocv_est > vbat && ocv_est > vbat + OCV_VBAT_MAX_DELTA_UV ? vbat + OCV_VBAT_MAX_DELTA_UV : ocv_est;
	if (est > V_CUTOFF_UV + CUTOFF_DELTA_UV && soc <= 0 && !at_cutoff) {
		soc = 1;
		last_ocv_uv = find_ocv_for_pc(temp, find_pc_for_soc(p, soc));
		fake_low = true;
	} else if (est < V_CUTOFF_UV + CUTOFF_DELTA_UV && soc > 0) {
		cutoff_count++;
	} else {
		cutoff_count = 0;
		fake_low = at_cutoff = false;
	}
	if (cutoff_count > CUTOFF_CONFIRM) {
		soc = 0;
		last_ocv_uv = find_ocv_for_pc(temp, find_pc_for_soc(p, soc));
		at_cutoff = true;
	}
	if (at_cutoff && soc > 0)
		soc = 0;
	if (fake_low && soc == 0 && status != BAT_CHARGING)
		soc = 1;
out:
	if (pmic_usb_present()) {
		if (now_sec() >= 30) {
			int sum = 0;
			for (int i = 0; i < 5; i++)
				sum += vbat_uv();
			if (sum / 5 < PROTECT_UV)
				bad_voltage_count = bad_voltage_count + 1 < PROTECT_COUNT ? bad_voltage_count + 1 : PROTECT_COUNT;
			else
				bad_voltage_count = 0;
			if (bad_voltage_count >= PROTECT_COUNT)
				soc = 0;
			else if (soc == 0)
				soc = 1;
		}
	} else {
		bad_voltage_count = 0;
	}
	return soc;
}

static void backup_soc(int soc)
{
	int ma = prev_uuc_iavg_ma;
	pmic_write(BMS_IAVG_STORAGE, ma > MIN_IAVG_MA ? (uint8_t)((ma - MIN_IAVG_MA) / IAVG_STEP_MA) : 0);
	uint8_t v = temp > 50 ? (uint8_t)((soc + 1) << 1) : 0xfe;
	pmic_write(BMS_SOC_STORAGE, (pmic_read(BMS_SOC_STORAGE) & 1) | v);
}

/* The reported value: rises only while charging, catching up with the
 * calculated one over up to ten minutes, and moves by 1 % at most every
 * 5 s, unless a long sleep came in between. */
static void report_soc(void)
{
	int soc = calculated_soc, charge_time = 0;
	uint32_t change = last_change_sec;
	int since = delta_time(&change);
	bool charging = status == BAT_CHARGING;
	if (charging) {
		if (!charge_start_sec) {
			catch_up_sec = iabs(soc - last_soc) < 10 ? (soc - last_soc) * 60 : 600;
			if (catch_up_sec < 0)
				catch_up_sec = 0;
			charge_start_sec = change;
		}
		charge_time = (int)(change - charge_start_sec) < 600 ? (int)(change - charge_start_sec) : 600;
		if (last_soc == soc)
			catch_up_sec = 0;
	}
	if (last_soc != NONE) {
		if (last_soc < soc && !(charging || (last_soc_unbound && was_charging)))
			soc = last_soc;
		else if (last_soc < soc && soc != 100 && catch_up_sec && charge_time <= catch_up_sec)
			soc = ((catch_up_sec - charge_time) * last_soc + charge_time * soc) / catch_up_sec;
		int step = iabs(last_soc - soc), max = low_voltage ? since : since / 5;
		if (step > max)
			step = max;
		if (last_soc_unbound)
			last_soc_unbound = false;
		else if (step > 1)
			step = 1;
		if (soc < last_soc && soc != 0)
			soc = last_soc - step;
		if (soc > last_soc && soc != 100)
			soc = last_soc + step;
	}
	if (last_soc != soc && !last_soc_unbound)
		last_change_sec = change;
	last_soc = bound_soc(soc);
	backup_soc(last_soc);
}

/* With first set only the raw value is worked out and returned, to
 * check the stored one against. */
static int recalculate(bool first)
{
	struct params p;
	int ocv_raw, soc;
	int64_t cc;
	temp = battery_temp();
	vadc_calibrate();
	read_soc_params(&ocv_raw, &cc);
	soc_params(cc, &p);
	if (p.fcc_uah - p.uuc_uah <= 0) {
		soc = 0;
	} else {
		soc = raw_soc(&p);
		if (first)
			return soc;
		if (first_soc && soc != shutdown_soc && !shutdown_soc_invalid) {
			last_ocv_uv = find_ocv_for_pc(temp, find_pc_for_soc(&p, shutdown_soc));
			p.ocv_charge_uah = find_ocv_charge_for_soc(&p, shutdown_soc);
			int ruc = p.ocv_charge_uah - p.cc_uah - p.uuc_uah, usable = p.fcc_uah - p.uuc_uah;
			soc = (ruc * 100 + (ruc < 0 ? -usable : usable) / 2) / usable;
		}
		if (!(first_soc && !shutdown_soc_invalid))
			soc = adjust_soc(&p, soc);
		if (status != BAT_CHARGING && soc <= 0 && vbat_uv() > V_CUTOFF_UV)
			soc = 1;
		soc = bound_soc(soc);
	}
	calculated_soc = soc;
	if (last_soc_invalid) {
		last_soc_invalid = false;
		last_soc = NONE;
	}
	if (p.delta_s * 1000 > CALC_MS * 15 && !first_soc) {
		last_soc_unbound = true;
		last_change_sec = last_recalc_sec;
	}
	report_soc();
	log_printf("bms: %d%% (calculated %d%%), ocv %d uV, cc %d uAh, fcc %d uAh, uuc %d uAh, iavg %d mA, rbatt %d mOhm, %d.%d C\n",
		   last_soc, calculated_soc, last_ocv_uv, p.cc_uah, p.fcc_uah, p.uuc_uah, p.iavg_ua / 1000, p.rbatt_mohm,
		   temp / 10, iabs(temp % 10));
	last_recalc_sec = now_sec();
	first_soc = first_uuc = false;
	was_charging = status == BAT_CHARGING;
	return soc;
}

static void status_check(void)
{
	int s = pmic_battery_status();
	if (s == status)
		return;
	if (s == BAT_CHARGING) {
		charge_start_sec = 0;
		catch_up_sec = 0;
		report_soc();
		soc_at_cv = prev_chg_soc = NONE;
	} else if (status == BAT_CHARGING) {
		charge_start_sec = 0;
		catch_up_sec = 0;
		report_soc();
		soc_at_cv = prev_chg_soc = NONE;
		if (s == BAT_FULL) {
			done_charging = true;
			last_soc_invalid = true;
		} else if (charging_adjusted_ocv > 0) {
			last_ocv_uv = charging_adjusted_ocv;
		}
		charging_adjusted_ocv = NONE;
	}
	status = s;
	recalculate(false);
}

void bms_init(void)
{
	int ohm = ratiometric_mv(CHAN_BATT_ID), kohm = ohm > 0 && ohm < 1800 ? 68 * ohm / (1800 - ohm) : 0;
	cell = iabs(cells[1].id_kohm - kohm) < iabs(cells[0].id_kohm - kohm) ? &cells[1] : &cells[0];
	uint8_t stored = pmic_read(BMS_SOC_STORAGE), iavg = pmic_read(BMS_IAVG_STORAGE);
	int stored_soc = (stored >> 1) ? (stored >> 1) - 1 : NONE;
	status = pmic_battery_status();
	usb_was = pmic_usb_present();
	shutdown_iavg_ma = MIN_IAVG_MA;
	int raw = recalculate(true);
	log_printf("bms: cell %s (id %d kOhm), stored %d%%, raw %d%%\n", cell->name, kohm, stored_soc, raw);
	shutdown_soc = stored_soc;
	shutdown_soc_invalid = stored_soc == NONE || stored_soc > 100 || iabs(stored_soc - raw) > SHUTDOWN_SOC_LIMIT;
	if (!shutdown_soc_invalid)
		shutdown_iavg_ma = iavg == 0 || iavg == 0xff ? MIN_IAVG_MA : MIN_IAVG_MA + IAVG_STEP_MA * iavg;
	recalculate(false);
	poll_sec = now_sec();
}

static int calc_ms(void)
{
	if (low_voltage)
		return LOW_VOLTAGE_CALC_MS;
	return calculated_soc < LOW_SOC ? LOW_SOC_CALC_MS : CALC_MS;
}

void bms_poll(void)
{
	uint32_t now = now_sec();
	if (now == poll_sec)
		return;
	poll_sec = now;
	bool usb = pmic_usb_present();
	if (usb != usb_was) {
		usb_was = usb;
		charger_gone_hold = !usb;
		charger_gone_sec = now;
	}
	status_check();
	if ((now - last_recalc_sec) * 1000 >= (uint32_t)calc_ms())
		recalculate(false);
}

/* The reported state of charge, not let fall by a few per cent while a
 * charger is in, nor in the first seconds after it is pulled. */
uint32_t bms_soc(void)
{
	return last_soc == NONE ? 50 : (uint32_t)last_soc;
}

uint32_t bms_percent(void)
{
	int soc = (int)bms_soc();
	bool usb = pmic_usb_present();
	if (held_soc == NONE || soc < 50) {
		held_soc = soc;
	} else if (usb && held_soc > soc && held_soc - soc < HOLD_DROP) {
		soc = held_soc;
	} else if (!usb && held_soc > soc && charger_gone_hold) {
		if (now_sec() - charger_gone_sec < CHARGER_GONE_HOLD_S) {
			soc = held_soc;
		} else {
			charger_gone_hold = false;
			held_soc = soc;
		}
	} else {
		held_soc = soc;
	}
	return (uint32_t)soc;
}
