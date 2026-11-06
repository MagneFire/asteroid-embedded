#include <string.h>
#include "m4_proto.h"

#define GESTURE_VIEW		1
#define GESTURE_WRIST_ROTATE	2
#define GESTURE_VIEW_ON		0

/* What the vendor's flashing leaves at the end of the firmware area: the
 * M4 must not be started without it. */
static const uint8_t barker[M4_BARKER_SIZE] = { 0xde, 0xc0, 0xce, 0x0a, 0, 0, 0, 0 };

void m4_bl_address(uint32_t addr, uint8_t out[5])
{
	out[0] = (uint8_t)(addr >> 24);
	out[1] = (uint8_t)(addr >> 16);
	out[2] = (uint8_t)(addr >> 8);
	out[3] = (uint8_t)addr;
	out[4] = out[0] ^ out[1] ^ out[2] ^ out[3];
}

bool m4_barker_ok(const uint8_t *b)
{
	return !memcmp(b, barker, sizeof barker);
}

uint32_t m4_u32(const uint8_t *b)
{
	return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

bool m4_is_tilt(uint32_t wake_status, uint8_t gesture, int8_t value)
{
	if (!(wake_status & M4_WAKE_GESTURE))
		return false;
	return gesture == GESTURE_WRIST_ROTATE || (gesture == GESTURE_VIEW && value == GESTURE_VIEW_ON);
}
