/* PM8226 vibrator: the motor driver inside the PMIC, peripheral 0xc0 on
 * SPMI slave 1. VTG_CTL sets the drive in 100 mV steps (bits 4:0, 1.2
 * to 3.1 V) and bit 7 of EN_CTL switches the motor. Its state survives
 * the watch's sleep and a warm reset, so init switches it off first. */
#include "asteroid.h"

#define VIB_TYPE		0x01c004
#define VIB_SUBTYPE		0x01c005
#define VIB_VTG_CTL		0x01c041
#define VIB_EN_CTL		0x01c046
#define VTG_LEVEL		0x1f
#define EN_MOTOR		BIT(7)

#define LEVEL_MIN		12	/* 1.2 V */
#define LEVEL_MAX		31	/* 3.1 V */

static bool pm8226_init(void)
{
	uint8_t level = (uint8_t)(board->motor_pins.vib_mv / 100);
	if (level < LEVEL_MIN || level > LEVEL_MAX)
		level = level < LEVEL_MIN ? LEVEL_MIN : LEVEL_MAX;
	pmic_write(VIB_EN_CTL, 0);
	uint8_t vtg = pmic_read(VIB_VTG_CTL);
	pmic_write(VIB_VTG_CTL, (uint8_t)((vtg & ~VTG_LEVEL) | level));
	vtg = pmic_read(VIB_VTG_CTL);
	uint8_t en = pmic_read(VIB_EN_CTL);
	log_printf("haptics: pm8226 vibrator type %02x.%02x vtg %02x en %02x, %u mV\n", pmic_read(VIB_TYPE),
		   pmic_read(VIB_SUBTYPE), vtg, en, level * 100);
	return (vtg & VTG_LEVEL) == level && !(en & EN_MOTOR);
}

static void pm8226_set(bool on)
{
	static bool told;
	uint8_t v = on ? EN_MOTOR : 0;
	int r = spmi_write(VIB_EN_CTL, &v, 1);
	uint8_t en = pmic_read(VIB_EN_CTL);
	if ((r || (en & EN_MOTOR) != v) && !told) {
		told = true;
		log_printf("haptics: motor %s not taken, write %d, en %02x vtg %02x\n", on ? "on" : "off", r, en,
			   pmic_read(VIB_VTG_CTL));
	}
}

const struct motor_driver motor_pm8226 = { .init = pm8226_init, .set = pm8226_set };
