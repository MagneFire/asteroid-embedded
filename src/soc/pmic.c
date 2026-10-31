/* PM8226 on SPMI slave 0. The PON block (peripheral 0x08) records why the
 * chip powered on and decides what a PS_HOLD drop does: configured here as
 * a warm reset so DRAM, and with it the persistent log, survives every
 * reboot the firmware or the watchdog causes. */
#include <string.h>
#include "asteroid.h"

#define REVID_TYPE		0x000104
#define REVID_SUBTYPE		0x000105
#define PON_REASON1		0x000808
#define PON_WARM_RESET_REASON1	0x00080a
#define PON_WARM_RESET_REASON2	0x00080b
#define PON_POFF_REASON1	0x00080c
#define PON_POFF_REASON2	0x00080d
#define PON_PS_HOLD_RESET_CTL	0x00085a
#define PON_PS_HOLD_RESET_CTL2	0x00085b
#define RESET_TYPE_WARM		0x1
#define RESET_TYPE_SHUTDOWN	0x4
#define S2_RESET_EN		BIT(7)
#define SMPS2_MODE		0x011745
#define SMPS_MODE_AUTO		0x40

static char reason[128];

void pmic_init(void)
{
	uint8_t type = pmic_read(REVID_TYPE), sub = pmic_read(REVID_SUBTYPE);
	uint8_t pon = pmic_read(PON_REASON1);
	uint8_t warm1 = pmic_read(PON_WARM_RESET_REASON1), warm2 = pmic_read(PON_WARM_RESET_REASON2);
	uint8_t poff1 = pmic_read(PON_POFF_REASON1), poff2 = pmic_read(PON_POFF_REASON2);
	snprintf(reason, sizeof reason, "pmic type %02x.%02x pon %02x warm %02x%02x poff %02x%02x",
		 type, sub, pon, warm1, warm2, poff1, poff2);

	pmic_write(PON_PS_HOLD_RESET_CTL2, 0);
	delay_us(300);
	pmic_write(PON_PS_HOLD_RESET_CTL, RESET_TYPE_WARM);
	pmic_write(PON_PS_HOLD_RESET_CTL2, S2_RESET_EN);
	snprintf(reason + strlen(reason), sizeof reason - strlen(reason), " ps_hold %02x/%02x",
		 pmic_read(PON_PS_HOLD_RESET_CTL), pmic_read(PON_PS_HOLD_RESET_CTL2));

	pmic_write(SMPS2_MODE, SMPS_MODE_AUTO);
}

const char *pmic_pon_reason(void) { return reason; }

/* Makes the next PS_HOLD drop power the watch off instead of resetting
 * it; the power key, or a charger, turns it back on. */
void pmic_shutdown_mode(void)
{
	pmic_write(PON_PS_HOLD_RESET_CTL2, 0);
	delay_us(300);
	pmic_write(PON_PS_HOLD_RESET_CTL, RESET_TYPE_SHUTDOWN);
	pmic_write(PON_PS_HOLD_RESET_CTL2, S2_RESET_EN);
}

/* Power key: the PON block's real-time status, bit 0 = KPDPWR_N low. */
#define PON_INT_RT_STS		0x000810

bool pmic_power_key(void)
{
	return pmic_read(PON_INT_RT_STS) & BIT(0);
}

/* The key also raises a PMIC interrupt on both edges, which reaches the
 * MPM and so wakes the watch from its deepest sleep. The interrupt
 * handler only masks it at the arbiter: SPMI transfers belong to the main
 * loop, which acknowledges the press and unmasks it again. */
#define PON_INT_SET_TYPE	0x000811
#define PON_INT_POLARITY_HIGH	0x000812
#define PON_INT_POLARITY_LOW	0x000813
#define PON_INT_LATCHED_CLR	0x000814
#define PON_INT_EN_SET		0x000815
#define PON_KPDPWR		BIT(0)

static int pon_apid = -1, usb_apid = -1;
static volatile bool key_irq, usb_irq;

/* The power key and the USB path share the PMIC's interrupt line; each
 * is masked until the main loop has handled it. One that comes with
 * neither status bit set masks both. */
static void spmi_periph_irq(void)
{
	bool key = pon_apid >= 0 && spmi_irq_status(pon_apid), usb = usb_apid >= 0 && spmi_irq_status(usb_apid);
	if (key || !usb) {
		if (pon_apid >= 0)
			spmi_irq_enable(pon_apid, false);
		key_irq = true;
	}
	if (usb || !key) {
		if (usb_apid >= 0)
			spmi_irq_enable(usb_apid, false);
		usb_irq = usb_apid >= 0;
	}
}

void pmic_power_key_irq_init(void)
{
	pon_apid = spmi_apid(0, 0x08);
	if (pon_apid < 0) {
		log_printf("pmic: no interrupt mapping for the power key\n");
		return;
	}
	pmic_write(PON_INT_SET_TYPE, pmic_read(PON_INT_SET_TYPE) | PON_KPDPWR);
	pmic_write(PON_INT_POLARITY_HIGH, pmic_read(PON_INT_POLARITY_HIGH) | PON_KPDPWR);
	pmic_write(PON_INT_POLARITY_LOW, pmic_read(PON_INT_POLARITY_LOW) | PON_KPDPWR);
	pmic_write(PON_INT_LATCHED_CLR, PON_KPDPWR);
	pmic_write(PON_INT_EN_SET, PON_KPDPWR);
	spmi_irq_clear(pon_apid, spmi_irq_status(pon_apid));
	gic_enable(IRQ_SPMI_PERIPH, spmi_periph_irq);
	spmi_irq_enable(pon_apid, true);
}

/* Whether the key interrupt fired since the last call. */
bool pmic_power_key_irq(void)
{
	if (!key_irq)
		return false;
	key_irq = false;
	pmic_write(PON_INT_LATCHED_CLR, PON_KPDPWR);
	spmi_irq_clear(pon_apid, spmi_irq_status(pon_apid));
	spmi_irq_enable(pon_apid, true);
	return true;
}

/* RTC: seconds since the epoch in four little-endian bytes; the counter
 * runs as long as the PMIC has power. Writes go through a stop/start of
 * the counter as the block requires. */
#define RTC_CTRL		0x006046
#define RTC_WRITE		0x006040
#define RTC_READ		0x006048
#define RTC_ENABLE		BIT(7)

uint32_t pmic_rtc_read(void)
{
	uint8_t b[4];
	spmi_read(RTC_READ, b, 4);
	uint32_t s = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
	if (b[0] == 0) {
		uint8_t c[4];
		spmi_read(RTC_READ, c, 4);
		if (c[0] != 0)
			s = c[0] | c[1] << 8 | c[2] << 16 | (uint32_t)c[3] << 24;
	}
	return s;
}

void pmic_rtc_write(uint32_t secs)
{
	uint8_t ctrl = pmic_read(RTC_CTRL);
	uint8_t b[4] = { (uint8_t)secs, (uint8_t)(secs >> 8), (uint8_t)(secs >> 16), (uint8_t)(secs >> 24) };
	pmic_write(RTC_CTRL, ctrl & ~RTC_ENABLE);
	spmi_write(RTC_WRITE, b, 4);
	pmic_write(RTC_CTRL, ctrl | RTC_ENABLE);
}

/* Voltage ADC: VBAT_SNS is the pack voltage divided by three; the
 * 0.625 V and 1.25 V references give the absolute calibration. settle is
 * the hardware settling time code, in steps of 100 us. */
#define VADC			0x003100
#define VADC_STATUS		(VADC + 0x08)
#define VADC_MODE_CTRL		(VADC + 0x40)
#define VADC_EN_CTL		(VADC + 0x46)
#define VADC_CHAN_SEL		(VADC + 0x48)
#define VADC_DIG_PARAM		(VADC + 0x50)
#define VADC_HW_SETTLE		(VADC + 0x51)
#define VADC_CONV_REQ		(VADC + 0x52)
#define VADC_FAST_AVG_CTL	(VADC + 0x5a)
#define VADC_DATA		(VADC + 0x60)
#define CHAN_VBAT_SNS		6
#define CHAN_REF_625MV		9
#define CHAN_REF_1250MV		10

int pmic_vadc_raw(uint8_t chan, uint8_t settle)
{
	pmic_write(VADC_MODE_CTRL, 0);
	pmic_write(VADC_CHAN_SEL, chan);
	pmic_write(VADC_DIG_PARAM, 1 << 2);
	pmic_write(VADC_HW_SETTLE, settle);
	pmic_write(VADC_FAST_AVG_CTL, 0);
	pmic_write(VADC_EN_CTL, BIT(7));
	pmic_write(VADC_CONV_REQ, BIT(7));
	for (int i = 0; i < 200; i++) {
		if ((pmic_read(VADC_STATUS) & 3) == 1) {
			uint8_t d[2];
			spmi_read(VADC_DATA, d, 2);
			pmic_write(VADC_EN_CTL, 0);
			return d[0] | d[1] << 8;
		}
		delay_us(100);
	}
	pmic_write(VADC_EN_CTL, 0);
	return -1;
}

uint32_t pmic_battery_mv(void)
{
	int r = pmic_vadc_raw(CHAN_VBAT_SNS, 0), c625 = pmic_vadc_raw(CHAN_REF_625MV, 0);
	int c1250 = pmic_vadc_raw(CHAN_REF_1250MV, 0);
	if (r < 0 || c625 < 0 || c1250 <= c625)
		return 0;
	int32_t mv = 625 + (int32_t)(r - c625) * 625 / (c1250 - c625);
	return (uint32_t)(mv * 3);
}

/* Charger (SMBB on PM8226): the charger, buck, battery interface, USB
 * path and misc peripherals at 0x1000, 0x1100, 0x1200, 0x1300 and 0x1600.
 * Set up in the order the vendor driver probes them, with the limits of
 * the HB442528EBC cell: 4.35 V, 300 mA fast charge, resume 100 mV below
 * full. Writes to the 0xd0-0xff test registers need SEC_ACCESS first.
 * The hardware end-of-charge logic is overridden, so the poll ends the
 * charge itself, trims the regulation voltage and restarts charging. */
#define SMBB_CHGR		0x001000
#define SMBB_BUCK		0x001100
#define SMBB_BAT_IF		0x001200
#define SMBB_USB		0x001300
#define SMBB_MISC		0x001600
#define SMBB_RT_STS		0x10
#define SMBB_SEC_ACCESS		0xd0
#define SMBB_SEC_UNLOCK		0xa5
#define CHGR_VDD_MAX		(SMBB_CHGR + 0x40)
#define CHGR_VDD_SAFE		(SMBB_CHGR + 0x41)
#define CHGR_IBAT_MAX		(SMBB_CHGR + 0x44)
#define CHGR_IBAT_SAFE		(SMBB_CHGR + 0x45)
#define CHGR_VIN_MIN		(SMBB_CHGR + 0x47)
#define CHGR_CHG_CTRL		(SMBB_CHGR + 0x49)
#define CHGR_CHG_FAILED		(SMBB_CHGR + 0x4a)
#define CHGR_IBAT_TERM		(SMBB_CHGR + 0x5b)
#define CHGR_VBAT_DET		(SMBB_CHGR + 0x5d)
#define CHGR_TCHG_MAX_EN	(SMBB_CHGR + 0x60)
#define CHGR_TCHG_MAX		(SMBB_CHGR + 0x61)
#define CHGR_WDOG_TIME		(SMBB_CHGR + 0x62)
#define CHGR_TRICKLE_CLAMP	(SMBB_CHGR + 0xe3)
#define CHGR_CHG_OVR0		(SMBB_CHGR + 0xed)
#define CHGR_COMP_OVR1		(SMBB_CHGR + 0xee)
#define BUCK_VBAT_REG_MODE	(SMBB_BUCK + 0x74)
#define BUCK_VCHG_OV		(SMBB_BUCK + 0x77)
#define BUCK_COMP_OVRIDE1	(SMBB_BUCK + 0xeb)
#define BUCK_CTRL_TRIM1		(SMBB_BUCK + 0xf1)
#define BAT_IF_PRES_STATUS	(SMBB_BAT_IF + 0x08)
#define BAT_IF_BPD_CTRL		(SMBB_BAT_IF + 0x48)
#define BAT_IF_VREF_THM_CTRL	(SMBB_BAT_IF + 0x4a)
#define BAT_IF_BATFET_CTRL4	(SMBB_BAT_IF + 0x93)
#define USB_OVP_CTL		(SMBB_USB + 0x42)
#define USB_IMAX		(SMBB_USB + 0x44)
#define USB_SUSP		(SMBB_USB + 0x47)
#define USB_ENUM_T_STOP		(SMBB_USB + 0x4e)
#define USB_OCP_THR		(SMBB_USB + 0x52)
#define USB_CHG_GONE_REV_BST	(SMBB_USB + 0xed)
#define MISC_REVISION2		(SMBB_MISC + 0x01)
#define MISC_BOOT_DONE		(SMBB_MISC + 0x42)
#define CHGR_RT_FAILED		BIT(6)
#define CHGR_RT_FAST		BIT(5)
#define CHGR_RT_TRICKLE		BIT(4)
#define CHGR_RT_VBAT_DET_LOW	BIT(0)
#define BUCK_RT_VDD_LOOP	BIT(6)
#define BAT_IF_RT_FET_ON	BIT(2)
#define BAT_IF_RT_TEMP_OK	BIT(1)
#define BAT_IF_RT_PRESENT	BIT(0)
#define USB_RT_CHG_GONE		BIT(2)
#define USB_RT_VALID		BIT(1)
#define CHG_EN			BIT(7)
#define CHG_ON_BAT_FORCE	BIT(0)
#define VDDMAX_MV		4350
#define VDDSAFE_MV		(VDDMAX_MV + 80)
#define VINMIN_MV		4370
#define IBATMAX_MA		300
#define IBATSAFE_MA		600
#define ITERM_MA		20
#define RESUME_DELTA_MV		100
#define VBATDET_ERR_MV		50
#define TCHG_MINS		150
#define USB_WALL_MA		500
#define RESUME_PERCENT		99
#define EOC_PERIOD_MS		10000
#define EOC_COUNT		10
#define TRIM_MAX_DELTA_MV	80

static uint8_t trim_center;
static int32_t vddmax_delta_mv;
static bool usb_present, fast_on, chg_done, chg_failed, resuming, eoc_active;
static unsigned eoc_count, vbat_low_count;
static uint32_t poll_ms, eoc_ms, arb_ms, done_ms;
static bool arb_wait, done_wait;

static void smbb_update(uint32_t addr, uint8_t mask, uint8_t val)
{
	pmic_write(addr, (uint8_t)((pmic_read(addr) & ~mask) | (val & mask)));
}

static void smbb_update_secure(uint32_t addr, uint8_t mask, uint8_t val)
{
	uint8_t v = (uint8_t)((pmic_read(addr) & ~mask) | (val & mask));
	pmic_write((addr & ~0xffu) + SMBB_SEC_ACCESS, SMBB_SEC_UNLOCK);
	pmic_write(addr, v);
}

static void charge_enable(bool on)
{
	smbb_update(CHGR_CHG_CTRL, CHG_EN, on ? CHG_EN : 0);
}

static void force_run_on_battery(bool on)
{
	if (pmic_read(BAT_IF_PRES_STATUS) & BIT(7))
		smbb_update(CHGR_CHG_CTRL, CHG_ON_BAT_FORCE, on ? CHG_ON_BAT_FORCE : 0);
}

static void vbatdet_set(int32_t mv)
{
	pmic_write(CHGR_VBAT_DET, (uint8_t)((mv - 3240) / 20));
}

static void vddmax_set(void)
{
	int32_t trim = trim_center + vddmax_delta_mv / 10;
	pmic_write(CHGR_VDD_MAX, (VDDMAX_MV - 3240) / 10);
	smbb_update_secure(BUCK_CTRL_TRIM1, 0xf0, (uint8_t)((trim < 0 ? 0 : trim > 15 ? 15 : trim) << 4));
}

static void vddmax_adjust(int32_t vbat_mv)
{
	int32_t delta = VDDMAX_MV - 4 - vbat_mv;
	if (delta > 0 && delta < 8)
		return;
	int32_t closest = (delta + (delta > 0 ? 5 : -5)) / 10 * 10;
	vddmax_delta_mv += closest;
	if (vddmax_delta_mv > TRIM_MAX_DELTA_MV)
		vddmax_delta_mv = TRIM_MAX_DELTA_MV;
	if (vddmax_delta_mv < -TRIM_MAX_DELTA_MV)
		vddmax_delta_mv = -TRIM_MAX_DELTA_MV;
	vddmax_set();
}

static void vbatdet_bypass(bool on)
{
	smbb_update_secure(CHGR_COMP_OVR1, 0xff, on ? 0x01 : 0);
}

static void usb_changed(bool present)
{
	usb_present = present;
	chg_done = false;
	chg_failed = false;
	if (present) {
		smbb_update(USB_ENUM_T_STOP, BIT(0), BIT(0));
		pmic_write(USB_IMAX, USB_WALL_MA / 100);
		smbb_update(BAT_IF_BATFET_CTRL4, 0xc0, 0);
		eoc_active = true;
		eoc_ms = time_ms();
	} else {
		smbb_update(USB_SUSP, BIT(0), 0);
		pmic_write(USB_IMAX, 0);
		smbb_update(BAT_IF_BATFET_CTRL4, 0xc0, 0x40);
		eoc_active = false;
	}
}

static bool charging_now(uint8_t chg, uint8_t bat)
{
	if (!(bat & BAT_IF_RT_TEMP_OK))
		return false;
	return ((chg & CHGR_RT_TRICKLE) && !(bat & BAT_IF_RT_FET_ON)) ||
	       ((chg & CHGR_RT_FAST) && (bat & BAT_IF_RT_FET_ON));
}

const char *pmic_charger_status(void)
{
	static char s[96];
	uint8_t chg = pmic_read(SMBB_CHGR + SMBB_RT_STS), bat = pmic_read(SMBB_BAT_IF + SMBB_RT_STS);
	uint8_t usb = pmic_read(SMBB_USB + SMBB_RT_STS);
	const char *state = "not charging";
	if (!(bat & BAT_IF_RT_TEMP_OK))
		state = "not charging, temperature out of range";
	else if (charging_now(chg, bat))
		state = chg & CHGR_RT_FAST ? "fast charging" : "trickle charging";
	else if ((chg & CHGR_RT_FAILED) || chg_failed)
		state = "charge failed";
	else if ((usb & USB_RT_VALID) && chg_done)
		state = "charged";
	snprintf(s, sizeof s, "usb %s%s, battery %s, %s",
		 usb & USB_RT_VALID ? "valid" : "absent", usb & USB_RT_CHG_GONE ? " (charger gone)" : "",
		 bat & BAT_IF_RT_PRESENT ? "present" : "absent", state);
	return s;
}

bool pmic_charging(void)
{
	return charging_now(pmic_read(SMBB_CHGR + SMBB_RT_STS), pmic_read(SMBB_BAT_IF + SMBB_RT_STS));
}

bool pmic_usb_present(void)
{
	return pmic_read(SMBB_USB + SMBB_RT_STS) & USB_RT_VALID;
}

/* The USB path's valid interrupt on its rising edge only, so a cable
 * plugged in wakes the watch from its deepest sleep too. */
#define USB_INT_SET_TYPE	(SMBB_USB + 0x11)
#define USB_INT_POLARITY_HIGH	(SMBB_USB + 0x12)
#define USB_INT_POLARITY_LOW	(SMBB_USB + 0x13)
#define USB_INT_LATCHED_CLR	(SMBB_USB + 0x14)
#define USB_INT_EN_SET		(SMBB_USB + 0x15)

void pmic_usb_irq_init(void)
{
	usb_apid = spmi_apid(0, SMBB_USB >> 8);
	if (usb_apid < 0) {
		log_printf("pmic: no interrupt mapping for USB\n");
		return;
	}
	pmic_write(USB_INT_SET_TYPE, pmic_read(USB_INT_SET_TYPE) | USB_RT_VALID);
	pmic_write(USB_INT_POLARITY_HIGH, pmic_read(USB_INT_POLARITY_HIGH) | USB_RT_VALID);
	pmic_write(USB_INT_POLARITY_LOW, pmic_read(USB_INT_POLARITY_LOW) & ~USB_RT_VALID);
	pmic_write(USB_INT_LATCHED_CLR, USB_RT_VALID);
	pmic_write(USB_INT_EN_SET, USB_RT_VALID);
	spmi_irq_clear(usb_apid, spmi_irq_status(usb_apid));
	spmi_irq_enable(usb_apid, true);
}

/* Whether the USB interrupt fired since the last call. */
bool pmic_usb_irq(void)
{
	if (!usb_irq)
		return false;
	usb_irq = false;
	pmic_write(USB_INT_LATCHED_CLR, USB_RT_VALID);
	spmi_irq_clear(usb_apid, spmi_irq_status(usb_apid));
	spmi_irq_enable(usb_apid, true);
	return true;
}

int pmic_battery_status(void)
{
	if (pmic_charging())
		return BAT_CHARGING;
	return pmic_usb_present() && chg_done ? BAT_FULL : BAT_DISCHARGING;
}

void pmic_charger_configure(void)
{
	smbb_update(CHGR_VIN_MIN, 0x3f, (uint8_t)(0x0c + (VINMIN_MV - 4000) / 50));
	pmic_write(CHGR_VDD_SAFE, (VDDSAFE_MV - 3240) / 10);
	vbatdet_set(VDDMAX_MV - RESUME_DELTA_MV);
	smbb_update(CHGR_IBAT_MAX, 0x3f, IBATMAX_MA / 50);
	smbb_update(CHGR_IBAT_TERM, 0x03, (ITERM_MA < 100 ? 0 : (ITERM_MA - 100) / 50));
	smbb_update(CHGR_IBAT_SAFE, 0x3f, IBATSAFE_MA / 50);
	smbb_update(CHGR_TCHG_MAX_EN, BIT(7), 0);
	smbb_update(CHGR_TCHG_MAX, 0x7f, TCHG_MINS / 4 - 1);
	smbb_update(CHGR_TCHG_MAX_EN, BIT(7), BIT(7));
	pmic_write(CHGR_WDOG_TIME, 0xa0);
	pmic_write(CHGR_IBAT_TERM, 0x08);
	smbb_update_secure(CHGR_CHG_OVR0, 0xff, 0);
	smbb_update_secure(CHGR_TRICKLE_CLAMP, 0xff, 0);

	smbb_update_secure(BUCK_COMP_OVRIDE1, 0xc0, 0x80);
	smbb_update(BUCK_VBAT_REG_MODE, BIT(0), BIT(0));
	trim_center = pmic_read(BUCK_CTRL_TRIM1) >> 4;
	smbb_update_secure(BUCK_VCHG_OV, 0xff, 0);

	smbb_update(BAT_IF_BPD_CTRL, 0x03, BIT(1));
	smbb_update(BAT_IF_VREF_THM_CTRL, 0xc0, 0xc0);

	smbb_update(USB_OVP_CTL, 0x03, 0x03);
	smbb_update(USB_ENUM_T_STOP, BIT(0), BIT(0));
	smbb_update_secure(USB_CHG_GONE_REV_BST, 0xff, 0x80);
	smbb_update(USB_OCP_THR, 0x03, 0);

	smbb_update(MISC_BOOT_DONE, BIT(7), BIT(7));

	charge_enable(true);
	force_run_on_battery(false);
	vddmax_delta_mv = 0;
	vddmax_set();
	usb_changed(pmic_read(SMBB_USB + SMBB_RT_STS) & USB_RT_VALID);
	poll_ms = time_ms();

	log_printf("charger: rev %02x boot %02x ctrl %02x vmax %u vsafe %u vbatdet %u imax %u isafe %u vinmin %u usb imax %u trim %u\n",
		   pmic_read(MISC_REVISION2), pmic_read(MISC_BOOT_DONE), pmic_read(CHGR_CHG_CTRL), pmic_read(CHGR_VDD_MAX),
		   pmic_read(CHGR_VDD_SAFE), pmic_read(CHGR_VBAT_DET), pmic_read(CHGR_IBAT_MAX), pmic_read(CHGR_IBAT_SAFE),
		   pmic_read(CHGR_VIN_MIN), pmic_read(USB_IMAX), trim_center);
}

/* Charger upkeep, at most once a second: follows USB plug and unplug,
 * restarts the charger after a charger-gone glitch or a failed charge,
 * ends the charge every ten seconds once the current in constant voltage
 * stays under the termination current, and restarts it when the state of
 * charge drops below 99 %. */
static void eoc_check(uint8_t chg, uint8_t bat)
{
	uint8_t buck = pmic_read(SMBB_BUCK + SMBB_RT_STS);
	charge_enable(true);
	if (!(bat & BAT_IF_RT_FET_ON) || !(chg & (CHGR_RT_FAST | CHGR_RT_TRICKLE))) {
		eoc_active = false;
		return;
	}
	int32_t ma = pmic_battery_ua() / 1000, mv = (int32_t)pmic_battery_mv();
	if (!(chg & CHGR_RT_VBAT_DET_LOW) && mv < VDDMAX_MV - RESUME_DELTA_MV - VBATDET_ERR_MV) {
		if (++vbat_low_count >= EOC_COUNT)
			eoc_active = false;
		return;
	}
	vbat_low_count = 0;
	if (buck & BUCK_RT_VDD_LOOP)
		vddmax_adjust(mv);
	if (!(buck & BUCK_RT_VDD_LOOP) || -ma > ITERM_MA || ma > 0) {
		eoc_count = 0;
		return;
	}
	if (eoc_count++ < EOC_COUNT)
		return;
	log_printf("charger: end of charge at %ld mV, %ld mA\n", mv, ma);
	chg_done = true;
	charge_enable(false);
	fast_on = false;
	vbatdet_bypass(false);
	done_wait = true;
	done_ms = time_ms();
	eoc_active = false;
}

void pmic_charger_poll(void)
{
	uint32_t now = time_ms();
	if (now - poll_ms < 1000)
		return;
	poll_ms = now;
	uint8_t chg = pmic_read(SMBB_CHGR + SMBB_RT_STS), bat = pmic_read(SMBB_BAT_IF + SMBB_RT_STS);
	uint8_t usb = pmic_read(SMBB_USB + SMBB_RT_STS);

	if (!!(usb & USB_RT_VALID) != usb_present)
		usb_changed(usb & USB_RT_VALID);

	if (done_wait && now - done_ms >= 2000) {
		done_wait = false;
		charge_enable(true);
	}

	if (arb_wait && now - arb_ms >= 1000) {
		arb_wait = false;
		if (!chg_done)
			charge_enable(true);
		force_run_on_battery(false);
	} else if (!arb_wait && usb_present && (usb & USB_RT_CHG_GONE)) {
		charge_enable(false);
		force_run_on_battery(true);
		arb_wait = true;
		arb_ms = now;
	}

	if (chg & CHGR_RT_FAILED) {
		log_printf("charger: charge failed, restarting\n");
		chg_failed = true;
		smbb_update(CHGR_CHG_FAILED, BIT(7), BIT(7));
	}

	bool fast = chg & CHGR_RT_FAST;
	if (fast != fast_on) {
		fast_on = fast;
		vbatdet_bypass(fast);
		if (fast) {
			chg_done = false;
			chg_failed = false;
			if (resuming) {
				resuming = false;
				vbatdet_set(VDDMAX_MV - RESUME_DELTA_MV);
			}
			eoc_active = true;
			eoc_ms = now;
			eoc_count = vbat_low_count = 0;
		}
	}
	if (chg & CHGR_RT_TRICKLE)
		chg_done = false;

	if (!usb_present || now - eoc_ms < EOC_PERIOD_MS)
		return;
	eoc_ms = now;
	if (eoc_active && !done_wait && !arb_wait) {
		eoc_check(chg, bat);
		if (!eoc_active)
			eoc_count = vbat_low_count = 0;
		return;
	}
	if (resuming && !(chg & (CHGR_RT_FAST | CHGR_RT_TRICKLE)))
		resuming = false;
	if (charging_now(chg, bat) || resuming || done_wait || arb_wait || !(bat & BAT_IF_RT_PRESENT))
		return;
	if (bms_soc() <= RESUME_PERCENT) {
		log_printf("charger: resuming at %lu%%\n", bms_soc());
		resuming = true;
		vbatdet_set(VDDMAX_MV + RESUME_DELTA_MV);
		charge_enable(true);
	}
}

/* Battery current through the current ADC across the external sense
 * resistor (9.7 mOhm on this watch): each reading is calibrated with a
 * gain conversion (17.857 mV reference) and an offset conversion of the
 * same input with its terminals shorted. */
#define IADC			0x003600
#define IADC_STATUS1		(IADC + 0x08)
#define IADC_MODE_CTL		(IADC + 0x40)
#define IADC_EN_CTL1		(IADC + 0x46)
#define IADC_CH_SEL		(IADC + 0x48)
#define IADC_DIG_PARAM		(IADC + 0x50)
#define IADC_HW_SETTLE		(IADC + 0x51)
#define IADC_CONV_REQ		(IADC + 0x52)
#define IADC_FAST_AVG_CTL	(IADC + 0x5a)
#define IADC_DATA		(IADC + 0x60)
#define IADC_CH_EXT_RSENSE	1
#define IADC_CH_GAIN		3
#define IADC_CH_EXT_OFFSET	5
#define IADC_GAIN_UV		17857
#define RSENSE_UOHM		9700

static int iadc_raw(uint8_t chan)
{
	pmic_write(IADC_MODE_CTL, BIT(0));
	pmic_write(IADC_CH_SEL, chan);
	pmic_write(IADC_DIG_PARAM, 0);
	pmic_write(IADC_HW_SETTLE, 0);
	pmic_write(IADC_FAST_AVG_CTL, 0);
	pmic_write(IADC_EN_CTL1, BIT(7));
	pmic_write(IADC_CONV_REQ, BIT(7));
	for (int i = 0; i < 100; i++) {
		delay_us(100);
		if ((pmic_read(IADC_STATUS1) & 3) == 1) {
			uint8_t d[2];
			spmi_read(IADC_DATA, d, 2);
			pmic_write(IADC_EN_CTL1, 0);
			return d[0] | d[1] << 8;
		}
	}
	pmic_write(IADC_EN_CTL1, 0);
	return -1;
}

/* Gain conversion minus offset conversion, or 0. */
int pmic_iadc_gain(void)
{
	int gain = iadc_raw(IADC_CH_GAIN), off = iadc_raw(IADC_CH_EXT_OFFSET);
	return gain < 0 || off < 0 || gain <= off ? 0 : gain - off;
}

/* Positive while the battery discharges. */
int32_t pmic_battery_ua(void)
{
	int gain = iadc_raw(IADC_CH_GAIN), off = iadc_raw(IADC_CH_EXT_OFFSET), raw = iadc_raw(IADC_CH_EXT_RSENSE);
	if (gain < 0 || off < 0 || raw < 0 || gain == off)
		return 0;
	int64_t uv = (int64_t)(int16_t)(raw - off) * IADC_GAIN_UV / (int16_t)(gain - off);
	return -(int32_t)(uv * 1000000 / RSENSE_UOHM);
}

/* Shadow coulomb counter of the battery monitor: a 36-bit signed sum of
 * the sense resistor voltage that the PMIC keeps up on its own, also
 * while the SoC sleeps. One count is 5.42535 uV over 56 sleep clock
 * cycles. Unlike the main counter, which the fuel gauge restarts, only
 * software clears it. The output registers are held while they are
 * read. */
#define BMS_CC_DATA_CTL		0x004042
#define BMS_SW_CC_DATA0		0x0040a8
#define BMS_HOLD_OREG_DATA	BIT(0)

static int64_t cc_raw(void)
{
	uint8_t d[5];
	pmic_write(BMS_CC_DATA_CTL, BMS_HOLD_OREG_DATA);
	delay_us(100);
	spmi_read(BMS_SW_CC_DATA0, d, 5);
	pmic_write(BMS_CC_DATA_CTL, 0);
	uint64_t v = d[0] | (uint64_t)d[1] << 8 | (uint64_t)d[2] << 16 | (uint64_t)d[3] << 24 | (uint64_t)(d[4] & 0xf) << 32;
	return (int64_t)(v << 28) >> 28;
}

/* Average since the previous call, positive while the battery discharges. */
int32_t pmic_battery_avg_ua(void)
{
	static int64_t last;
	static uint32_t last_ms;
	int64_t cc = cc_raw();
	uint32_t now = time_ms(), ms = now - last_ms;
	int64_t uas = (cc - last) * 542535 * 56 / 32764 * 10 / RSENSE_UOHM;
	bool first = !last_ms;
	last = cc;
	last_ms = now;
	return first || !ms ? 0 : (int32_t)(uas * 1000 / ms);
}
