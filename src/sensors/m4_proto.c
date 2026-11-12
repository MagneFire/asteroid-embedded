#include <string.h>
#include "m4_proto.h"

#define GESTURE_VIEW		1
#define GESTURE_WRIST_ROTATE	2
#define GESTURE_VIEW_ON		0

/* The mark is what the vendor's flashing leaves at the end of the
 * firmware area: the M4 must not be started without it. */
const struct m4_variant m4_smelt = {
	.start = M4_FLASH_BASE, .mark_addr = M4_FLASH_BASE + 512 * 1024 - 8,
	.mark = { 0xde, 0xc0, 0xce, 0x0a, 0, 0, 0, 0 }, .mark_size = 8,
	.general = 1, .pedometer = 3, .mets = 9, .power = 11, .gesture = 13, .profile = 10,
	.accel_x = 0x08, .version_size = 2,
};

/* The firmware is in the upper half of the flash. */
const struct m4_variant m4_minnow = {
	.start = M4_FLASH_BASE + 256 * 1024, .mark_addr = M4_FLASH_BASE + 512 * 1024 - 4,
	.mark = { 0xde, 0xc0, 0xce, 0x0a }, .mark_size = 4,
	.general = 2, .pedometer = 4, .mets = 10, .power = 12, .gesture = 18, .settings = 11, .light = 23, .profile = 11,
	.accel_x = 0x04, .version_size = 1,
	.byte_irqs = true, .drives_reset = true, .keeps_time = true,
};

/* The older hub's interrupt bits: gesture in the first byte, still and
 * motion in the second. */
#define INT0_GESTURE	(1u << 7)
#define INT1_STILL	(1u << 8)
#define INT1_MOTION	(1u << 9)

void m4_bl_address(uint32_t addr, uint8_t out[5])
{
	out[0] = (uint8_t)(addr >> 24);
	out[1] = (uint8_t)(addr >> 16);
	out[2] = (uint8_t)(addr >> 8);
	out[3] = (uint8_t)addr;
	out[4] = out[0] ^ out[1] ^ out[2] ^ out[3];
}

bool m4_barker_ok(const struct m4_variant *v, const uint8_t *b)
{
	return !memcmp(b, v->mark, v->mark_size);
}

uint32_t m4_u32(const uint8_t *b)
{
	return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

void m4_put_u16(uint16_t v, uint8_t *b)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
}

void m4_put_u32(uint32_t v, uint8_t *b)
{
	m4_put_u16((uint16_t)v, b);
	m4_put_u16((uint16_t)(v >> 16), b + 2);
}

bool m4_is_tilt(uint32_t wake_status, uint8_t gesture, int8_t value)
{
	if (!(wake_status & M4_WAKE_GESTURE))
		return false;
	return gesture == GESTURE_WRIST_ROTATE || (gesture == GESTURE_VIEW && value == GESTURE_VIEW_ON);
}

uint32_t m4_wake_mask(bool tilt)
{
	return M4_WAKE_STILL | M4_WAKE_MOTION | (tilt ? M4_WAKE_GESTURE : 0);
}

uint32_t m4_irq_enable(const struct m4_variant *v, bool tilt)
{
	if (!v->byte_irqs)
		return m4_wake_mask(tilt);
	return INT1_STILL | INT1_MOTION | (tilt ? INT0_GESTURE : 0);
}

uint32_t m4_irq_wake(const struct m4_variant *v, uint32_t status)
{
	if (!v->byte_irqs)
		return status;
	return (status & INT1_STILL ? M4_WAKE_STILL : 0) | (status & INT1_MOTION ? M4_WAKE_MOTION : 0) |
	       (status & INT0_GESTURE ? M4_WAKE_GESTURE : 0) |
	       (status & ~(INT0_GESTURE | INT1_STILL | INT1_MOTION) ? M4_WAKE_OTHER : 0);
}

uint32_t m4_block_steps(const uint8_t *block)
{
	return m4_u32(block + (M4_PEDOMETER_STEPS - M4_PEDOMETER_BLOCK));
}

uint32_t m4_block_distance(const uint8_t *block)
{
	return m4_u32(block + (M4_PEDOMETER_DISTANCE - M4_PEDOMETER_BLOCK));
}

bool m4_panicked(const uint8_t *reply)
{
	return m4_u32(reply) == M4_PANIC_MAGIC;
}
