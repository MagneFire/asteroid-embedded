/* The M4 sensor hub's I2C protocols, no hardware involved. In normal
 * running a register is a bank and an offset, little endian data after
 * them. The ROM bootloader takes opcodes with their inverse, then a big
 * endian address with an XOR of its bytes, and answers ACK, NACK or BUSY
 * to each. A hub that has crashed answers every register read with
 * zeros, and the panic probe with a magic word. */
#ifndef M4_PROTO_H
#define M4_PROTO_H
#include <stdbool.h>
#include <stdint.h>

#define M4_BL_ACK		0x79
#define M4_BL_NACK		0x1f
#define M4_BL_BUSY		0x76
#define M4_BL_READ		0x11
#define M4_BL_GO		0x21
#define M4_FLASH_BASE		0x08000000u
#define M4_MARK_MAX		8

/* What differs between the hub of the 2015 watch and that of the 2014
 * one, an older firmware on a smaller chip: where the firmware starts
 * and what marks a complete one at the end of the flash, the numbers of
 * the register banks, and the interrupts, one 32-bit wake enable and
 * status against three bytes of each. The older hub's reset line has to
 * be driven high, it keeps the wall clock, is told whether the screen
 * is on and has the light sensor. */
struct m4_variant {
	uint32_t start, mark_addr;
	uint8_t mark[M4_MARK_MAX], mark_size;
	uint8_t general, pedometer, mets, power, gesture;
	uint8_t settings, light;	/* 0 for a hub without */
	uint8_t profile;	/* the user settings bank, as settings without the screen */
	uint8_t accel_x, version_size;
	bool byte_irqs, drives_reset, keeps_time;
};
extern const struct m4_variant m4_smelt, m4_minnow;

#define M4_BANK_ACCEL		0
#define M4_BANK_PANIC		0xff
#define M4_GENERAL_UTC		0x00
#define M4_GENERAL_VERSION	0x0a
#define M4_GENERAL_NOWAKE_ENABLE	0x0c
#define M4_GENERAL_WAKE_ENABLE	0x10
#define M4_GENERAL_NOWAKE_STATUS	0x14
#define M4_GENERAL_WAKE_STATUS	0x18
/* The older hub's three enable bytes and three status bytes. */
#define M4_GENERAL_INT_ENABLE	0x0b
#define M4_GENERAL_INT_STATUS	0x0e
#define M4_SETTINGS_SCREEN	0x00
#define M4_SCREEN_OFF		0
#define M4_SCREEN_ON		1
#define M4_LIGHT_RATE		0x02
#define M4_LIGHT_VALUE		0x04
#define M4_PEDOMETER_ENABLE	0x00
/* Activity, equipment type, the step count, then the distances: read as
 * one block, so the hub never sees a request for the count itself (see
 * m4_steps). */
#define M4_PEDOMETER_BLOCK	0x02
#define M4_PEDOMETER_BLOCK_SIZE	18
#define M4_PEDOMETER_STEPS	0x04
#define M4_PEDOMETER_DISTANCE	0x10
#define M4_METS_ENABLE		0x00
#define M4_METS_CALORIES	0x14
/* Age, gender (1 male), height in cm, weight in kg: one byte each, as the
 * vendor writes them. */
#define M4_PROFILE_AGE		0x01
#define M4_POWER_ENABLE		0x01
#define M4_POWER_STILL_TIMEOUT	0x02
#define M4_POWER_STATE		0x08
#define M4_GESTURE_FIRST	0x01
#define M4_ACCEL_RATE		0x02
#define M4_PANIC_PROBE		0xcd
#define M4_PANIC_MAGIC		0xdeadbeefu

/* Wake interrupt bits: the hub raises its wake line for the ones
 * enabled. These are the newer hub's; the older one's bytes are turned
 * into them, with anything else it reports as M4_WAKE_OTHER. */
#define M4_WAKE_STILL		(1u << 0)
#define M4_WAKE_MOTION		(1u << 1)
#define M4_WAKE_GESTURE		(1u << 2)
#define M4_WAKE_OTHER		(1u << 31)
/* The vendor's still mode timeout, in seconds. */
#define M4_STILL_TIMEOUT_S	600

void m4_bl_address(uint32_t addr, uint8_t out[5]);
bool m4_barker_ok(const struct m4_variant *v, const uint8_t *b);
uint32_t m4_u32(const uint8_t *b);
void m4_put_u16(uint16_t v, uint8_t *b);
void m4_put_u32(uint32_t v, uint8_t *b);
/* A wrist turn or a look at the watch, from the wake status and the
 * first gesture and its value: looking away is a gesture too. */
bool m4_is_tilt(uint32_t wake_status, uint8_t gesture, int8_t value);
/* The wake interrupts to enable: the hub's still and motion states
 * always, so they can be followed, the gesture only when wanted. */
uint32_t m4_wake_mask(bool tilt);
/* The same for the hub's own registers, and what it reports back from
 * them as the bits above. The older hub's three bytes count from the
 * first up. */
uint32_t m4_irq_enable(const struct m4_variant *v, bool tilt);
uint32_t m4_irq_wake(const struct m4_variant *v, uint32_t status);
/* The step count and the distance out of the pedometer block read from
 * M4_PEDOMETER_BLOCK. */
uint32_t m4_block_steps(const uint8_t *block);
uint32_t m4_block_distance(const uint8_t *block);
/* The answer to the panic probe says the hub has crashed. */
bool m4_panicked(const uint8_t *reply);
#endif
