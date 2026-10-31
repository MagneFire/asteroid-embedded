/* PM8226 vibrator: the motor driver inside the PMIC, peripheral 0xc0 on
 * SPMI slave 1. VTG_CTL sets the drive in 100 mV steps (bits 4:0, 1.2
 * to 3.1 V) and bit 7 of EN_CTL switches the motor. Its state survives
 * the watch's sleep and a warm reset, so init switches it off first. */
#include "asteroid.h"
#include "input/haptics.h"

#define VIB_TYPE		0x01c004
#define VIB_SUBTYPE		0x01c005
#define VIB_VTG_CTL		0x01c041
#define VIB_EN_CTL		0x01c046
#define VTG_LEVEL		0x1f
#define EN_MOTOR		BIT(7)

#define LEVEL			(BOARD_VIB_MV / 100)
_Static_assert(LEVEL >= 12 && LEVEL <= 31, "BOARD_VIB_MV out of the vibrator's 1200-3100 mV");

bool haptics_motor_init(void)
{
	pmic_write(VIB_EN_CTL, 0);
	uint8_t vtg = pmic_read(VIB_VTG_CTL);
	pmic_write(VIB_VTG_CTL, (uint8_t)((vtg & ~VTG_LEVEL) | LEVEL));
	vtg = pmic_read(VIB_VTG_CTL);
	uint8_t en = pmic_read(VIB_EN_CTL);
	log_printf("haptics: pm8226 vibrator type %02x.%02x vtg %02x en %02x, %u mV\n", pmic_read(VIB_TYPE),
		   pmic_read(VIB_SUBTYPE), vtg, en, LEVEL * 100);
	return (vtg & VTG_LEVEL) == LEVEL && !(en & EN_MOTOR);
}

void haptics_motor(bool on)
{
	pmic_write(VIB_EN_CTL, on ? EN_MOTOR : 0);
}
