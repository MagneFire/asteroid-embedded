/* The M4 sensor hub's I2C protocols, no hardware involved. In normal
 * running a register is a bank and an offset, little endian data after
 * them. The ROM bootloader takes opcodes with their inverse, then a big
 * endian address with an XOR of its bytes, and answers ACK, NACK or BUSY
 * to each. */
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

#define M4_BANK_GENERAL		1
#define M4_BANK_PEDOMETER	3
#define M4_BANK_METS		9
#define M4_BANK_GESTURE		13
#define M4_GENERAL_UTC		0x00
#define M4_GENERAL_VERSION	0x0a
#define M4_GENERAL_WAKE_ENABLE	0x10
#define M4_GENERAL_WAKE_STATUS	0x18
#define M4_PEDOMETER_ENABLE	0x00
#define M4_PEDOMETER_STEPS	0x04
#define M4_METS_ENABLE		0x00
#define M4_GESTURE_FIRST	0x01

#define M4_WAKE_GESTURE		(1u << 2)

void m4_bl_address(uint32_t addr, uint8_t out[5]);
bool m4_barker_ok(const uint8_t *b);
uint32_t m4_u32(const uint8_t *b);
/* A wrist turn or a look at the watch, from the wake status and the
 * first gesture. */
bool m4_is_tilt(uint32_t wake_status, uint8_t gesture);
#endif
