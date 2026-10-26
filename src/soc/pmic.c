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
#define S2_RESET_EN		BIT(7)

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
}

const char *pmic_pon_reason(void) { return reason; }

/* Power key: the PON block's real-time status, bit 0 = KPDPWR_N low. */
#define PON_INT_RT_STS		0x000810

bool pmic_power_key(void)
{
	return pmic_read(PON_INT_RT_STS) & BIT(0);
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

/* Battery voltage through the voltage ADC: VBAT_SNS is the pack voltage
 * divided by three; the 0.625 V and 1.25 V references give the absolute
 * calibration. */
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

static int vadc_raw(uint8_t chan)
{
	pmic_write(VADC_MODE_CTRL, 0);
	pmic_write(VADC_CHAN_SEL, chan);
	pmic_write(VADC_DIG_PARAM, 1 << 2);
	pmic_write(VADC_HW_SETTLE, 0);
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
	int r = vadc_raw(CHAN_VBAT_SNS), c625 = vadc_raw(CHAN_REF_625MV), c1250 = vadc_raw(CHAN_REF_1250MV);
	if (r < 0 || c625 < 0 || c1250 <= c625)
		return 0;
	int32_t mv = 625 + (int32_t)(r - c625) * 625 / (c1250 - c625);
	return (uint32_t)(mv * 3);
}

/* Charger (SMBB): real-time status bits of the charger, battery interface
 * and USB path peripherals; limits as the battery's profile asks. */
#define SMBB_CHGR_RT_STS	0x001010
#define SMBB_BAT_IF_RT_STS	0x001210
#define SMBB_USB_RT_STS		0x001310
#define SMBB_CHG_VMAX		0x001040
#define SMBB_CHG_VSAFE		0x001041
#define SMBB_CHG_IMAX		0x001044
#define SMBB_CHG_ISAFE		0x001045
#define SMBB_CHG_VIN_MIN	0x001047
#define SMBB_USB_IMAX		0x001344

const char *pmic_charger_status(void)
{
	static char s[96];
	uint8_t chg = pmic_read(SMBB_CHGR_RT_STS), bat = pmic_read(SMBB_BAT_IF_RT_STS), usb = pmic_read(SMBB_USB_RT_STS);
	snprintf(s, sizeof s, "usb %s%s, battery %s, %s%s%s",
		 usb & BIT(1) ? "valid" : "absent", usb & BIT(2) ? " (charger gone)" : "",
		 bat & BIT(0) ? "present" : "absent",
		 chg & BIT(7) ? "charge done" : chg & BIT(5) ? "fast charging" : chg & BIT(4) ? "trickle charging" : "not charging",
		 bat & BIT(1) ? "" : ", temperature out of range",
		 "");
	return s;
}

void pmic_charger_configure(void)
{
	pmic_write(SMBB_CHG_VSAFE, (4370 - 3240) / 10);
	pmic_write(SMBB_CHG_VMAX, (4350 - 3240) / 10);
	pmic_write(SMBB_CHG_ISAFE, 600 / 50);
	pmic_write(SMBB_CHG_IMAX, 300 / 50);
	pmic_write(SMBB_CHG_VIN_MIN, (4350 - 3400) / 50);
	pmic_write(SMBB_USB_IMAX, 5);
	log_printf("charger: vmax %u vsafe %u imax %u isafe %u vinmin %u usb imax %u\n",
		   pmic_read(SMBB_CHG_VMAX), pmic_read(SMBB_CHG_VSAFE), pmic_read(SMBB_CHG_IMAX),
		   pmic_read(SMBB_CHG_ISAFE), pmic_read(SMBB_CHG_VIN_MIN), pmic_read(SMBB_USB_IMAX));
}

bool pmic_charging(void)
{
	uint8_t chg = pmic_read(SMBB_CHGR_RT_STS);
	return (pmic_read(SMBB_USB_RT_STS) & BIT(1)) && (chg & (BIT(5) | BIT(4) | BIT(7)));
}

/* Rough state of charge from the resting voltage of a 4.35 V cell, until
 * the fuel gauge is read: linear between 3.45 V (0 %) and 4.30 V (100 %). */
uint32_t pmic_battery_percent(uint32_t mv)
{
	if (mv <= 3450)
		return 0;
	if (mv >= 4300)
		return 100;
	return (mv - 3450) * 100 / (4300 - 3450);
}
