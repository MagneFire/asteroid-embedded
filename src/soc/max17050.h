/* Decoding the MAX17050 fuel gauge's registers, no hardware involved. */
#ifndef MAX17050_H
#define MAX17050_H
#include <stdbool.h>
#include <stdint.h>

#define MAX17050_STATUS		0x00
#define MAX17050_REPSOC		0x06
#define MAX17050_TEMP		0x08
#define MAX17050_VCELL		0x09
#define MAX17050_CURRENT	0x0a
#define MAX17050_AVGCURRENT	0x0b
#define MAX17050_DEVNAME	0x21
#define MAX17050_DEVNAME_ID	0x00ac

uint32_t max17050_percent(uint16_t repsoc);
uint32_t max17050_mv(uint16_t vcell);
/* Positive while discharging, as the battery driver table has it. */
int32_t max17050_ua(uint16_t current, uint32_t rsense_uohm);
/* The cell's temperature in tenths of a degree. */
int max17050_decidegrees(uint16_t temp);
/* The status register says the gauge lost power and its cell model, or
 * the battery is gone. */
bool max17050_lost_model(uint16_t status);
bool max17050_no_battery(uint16_t status);
#endif
