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
#define M4_BARKER_ADDR		(M4_FLASH_BASE + 512 * 1024 - 8)
#define M4_BARKER_SIZE		8

#define M4_BANK_ACCEL		0
#define M4_BANK_GENERAL		1
#define M4_BANK_PEDOMETER	3
#define M4_BANK_METS		9
#define M4_BANK_POWER		11
#define M4_BANK_GESTURE		13
#define M4_BANK_PANIC		0xff
#define M4_GENERAL_UTC		0x00
#define M4_GENERAL_VERSION	0x0a
#define M4_GENERAL_NOWAKE_ENABLE	0x0c
#define M4_GENERAL_WAKE_ENABLE	0x10
#define M4_GENERAL_NOWAKE_STATUS	0x14
#define M4_GENERAL_WAKE_STATUS	0x18
#define M4_PEDOMETER_ENABLE	0x00
/* Activity, equipment type, then the step count: read as one block, so
 * the hub never sees a request for the count itself (see m4_steps). */
#define M4_PEDOMETER_BLOCK	0x02
#define M4_PEDOMETER_BLOCK_SIZE	6
#define M4_PEDOMETER_STEPS	0x04
#define M4_METS_ENABLE		0x00
#define M4_POWER_ENABLE		0x01
#define M4_POWER_STILL_TIMEOUT	0x02
#define M4_POWER_STATE		0x08
#define M4_GESTURE_FIRST	0x01
#define M4_ACCEL_RATE		0x02
#define M4_ACCEL_X		0x08
#define M4_PANIC_PROBE		0xcd
#define M4_PANIC_MAGIC		0xdeadbeefu

/* Wake interrupt bits: the hub raises its wake line for the ones enabled. */
#define M4_WAKE_STILL		(1u << 0)
#define M4_WAKE_MOTION		(1u << 1)
#define M4_WAKE_GESTURE		(1u << 2)
/* The vendor's still mode timeout, in seconds. */
#define M4_STILL_TIMEOUT_S	600

void m4_bl_address(uint32_t addr, uint8_t out[5]);
bool m4_barker_ok(const uint8_t *b);
uint32_t m4_u32(const uint8_t *b);
void m4_put_u16(uint16_t v, uint8_t *b);
void m4_put_u32(uint32_t v, uint8_t *b);
/* A wrist turn or a look at the watch, from the wake status and the
 * first gesture and its value: looking away is a gesture too. */
bool m4_is_tilt(uint32_t wake_status, uint8_t gesture, int8_t value);
/* The wake interrupts to enable: the hub's still and motion states
 * always, so they can be followed, the gesture only when wanted. */
uint32_t m4_wake_mask(bool tilt);
/* The step count out of the pedometer block read from M4_PEDOMETER_BLOCK. */
uint32_t m4_block_steps(const uint8_t *block);
/* The answer to the panic probe says the hub has crashed. */
bool m4_panicked(const uint8_t *reply);
#endif
