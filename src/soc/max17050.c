/* The gauge's registers in the units the firmware uses: state of charge
 * in 1/256 %, voltage in 78.125 uV, current in 1.5625 uV across the
 * sense resistor (charging is positive) and temperature in 1/256 degree.
 * The thermistor reads off by a few degrees, so its reading goes through
 * the vendor's table of real temperatures. */
#include "max17050.h"

#define STATUS_POR	(1u << 1)
#define STATUS_BST	(1u << 3)

#define TEMP_START	(-20)
static const int16_t temp_real[] = {
	-302, -271, -246, -222, -202, -180, -162, -147, -129, -114,
	-101, -86, -72, -59, -48, -35, -23, -12, -2, 10,
	22, 32, 42, 51, 62, 72, 82, 91, 99, 110,
	120, 129, 138, 146, 156, 165, 175, 184, 192, 200,
	210, 220, 230, 238, 247, 256, 266, 275, 285, 294,
	303, 313, 324, 333, 343, 353, 364, 375, 385, 395,
	407, 419, 430, 441, 453, 466, 479, 492, 505, 520,
	534, 548, 565, 581, 597, 616, 635, 654, 676, 697,
	721, 747, 775, 800,
};
#define TEMP_N	((int)(sizeof temp_real / sizeof temp_real[0]))

uint32_t max17050_percent(uint16_t repsoc)
{
	uint32_t p = repsoc >> 8;
	return p > 100 ? 100 : p;
}

uint32_t max17050_mv(uint16_t vcell)
{
	return (uint32_t)vcell * 625 / 8 / 1000;
}

int32_t max17050_ua(uint16_t current, uint32_t rsense_uohm)
{
	return (int32_t)(-(int64_t)(int16_t)current * 1562500 / (int64_t)rsense_uohm);
}

int max17050_decidegrees(uint16_t temp)
{
	int t = (int16_t)temp * 10 / 256;
	int i = t / 10 - TEMP_START - (t < 0);
	if (i < 0)
		return temp_real[0];
	if (i >= TEMP_N - 1)
		return temp_real[TEMP_N - 1];
	return temp_real[i] + (temp_real[i + 1] - temp_real[i]) * (t - (TEMP_START + i) * 10) / 10;
}

bool max17050_lost_model(uint16_t status)
{
	return status & STATUS_POR;
}

bool max17050_no_battery(uint16_t status)
{
	return status & STATUS_BST;
}
