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
