/* Decoding the MAX17050 fuel gauge's registers, no hardware involved. */
#ifndef MAX17050_H
#define MAX17050_H
#include <stdbool.h>
#include <stdint.h>

#define MAX17050_STATUS		0x00
#define MAX17050_REPCAP		0x05
#define MAX17050_REPSOC		0x06
#define MAX17050_TEMP		0x08
#define MAX17050_VCELL		0x09
#define MAX17050_CURRENT	0x0a
#define MAX17050_AVGCURRENT	0x0b
#define MAX17050_DEVNAME	0x21
#define MAX17050_DEVNAME_ID	0x00ac
#define MAX17050_REMCAP		0x0f
#define MAX17050_FULLCAP	0x10
#define MAX17050_QRTBL00	0x12
#define MAX17050_FULLSOCTHR	0x13
#define MAX17050_CYCLES		0x17
#define MAX17050_DESIGNCAP	0x18
#define MAX17050_CONFIG		0x1d
#define MAX17050_ICHGTERM	0x1e
#define MAX17050_CONFIG_VER	0x20
#define MAX17050_QRTBL10	0x22
#define MAX17050_FULLCAPNOM	0x23
#define MAX17050_LEARNCFG	0x28
#define MAX17050_FILTERCFG	0x29
#define MAX17050_RELAXCFG	0x2a
#define MAX17050_TGAIN		0x2c
#define MAX17050_TOFF		0x2d
#define MAX17050_QRTBL20	0x32
#define MAX17050_RCOMP0		0x38
#define MAX17050_TEMPCO		0x39
#define MAX17050_QRTBL30	0x42
#define MAX17050_DQACC		0x45
#define MAX17050_DPACC		0x46
#define MAX17050_VFSOC0		0x48
#define MAX17050_QH		0x4d
#define MAX17050_VFSOC0_ENABLE	0x60
#define MAX17050_MLOCK1		0x62
#define MAX17050_MLOCK2		0x63
#define MAX17050_MODEL		0x80
#define MAX17050_VFSOC		0xff

#define MAX17050_MODEL_WORDS	48
#define MAX17050_VFSOC0_UNLOCK	0x0080
#define MAX17050_MLOCK1_UNLOCK	0x0059
#define MAX17050_MLOCK2_UNLOCK	0x00c4
/* Cycles after a fresh model, past which the gauge counts charge rather
 * than following the voltage. */
#define MAX17050_INIT_CYCLES	96
#define MAX17050_DPACC_200	0x0c80

/* The cell model the vendor's driver loads after the gauge lost power:
 * the configuration registers, the capacities in the gauge's units
 * (5 uVh across the sense resistor), the characterisation table and the
 * version it stamps into a spare register. */
struct max17050_model {
	uint16_t config, learn_cfg, filter_cfg, relax_cfg, full_soc_thresh;
	uint16_t design_cap, fullcap, fullcapnom, ichgt_term;
	uint16_t rcomp0, tempco, qrtbl[4], tgain, toff, version;
	uint16_t cell[MAX17050_MODEL_WORDS];
};

/* The capacity left for a fresh model from the voltage-only state of
 * charge, and the charge accumulator seed, as the vendor's driver sets them. */
uint16_t max17050_remcap(uint16_t vfsoc, uint16_t fullcapnom);
uint16_t max17050_dqacc(uint16_t fullcap);

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
