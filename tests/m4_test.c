/* The M4 hub's protocol helpers. */
#include <stdio.h>
#include <string.h>
#include "sensors/m4_proto.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

int main(void)
{
	uint8_t a[5];
	m4_bl_address(M4_FLASH_BASE, a);
	CHECK(!memcmp(a, "\x08\x00\x00\x00\x08", 5));
	m4_bl_address(M4_BARKER_ADDR, a);
	CHECK(M4_BARKER_ADDR == 0x0807fff8);
	CHECK(!memcmp(a, "\x08\x07\xff\xf8", 4) && a[4] == (0x08 ^ 0x07 ^ 0xff ^ 0xf8));

	uint8_t good[8] = { 0xde, 0xc0, 0xce, 0x0a, 0, 0, 0, 0 }, bad[8] = { 0xde, 0xc0, 0xce, 0x0b, 0, 0, 0, 0 };
	CHECK(m4_barker_ok(good));
	CHECK(!m4_barker_ok(bad));

	uint8_t n[4] = { 0x78, 0x56, 0x34, 0x12 };
	CHECK(m4_u32(n) == 0x12345678);

	CHECK(m4_is_tilt(M4_WAKE_GESTURE, 1, 0));
	CHECK(!m4_is_tilt(M4_WAKE_GESTURE, 1, 1));
	CHECK(m4_is_tilt(M4_WAKE_GESTURE, 2, 3));
	/* A tap is not a tilt, nor a gesture left from before. */
	CHECK(!m4_is_tilt(M4_WAKE_GESTURE, 3, 0));
	CHECK(!m4_is_tilt(0, 1, 0));

	puts(failures ? "m4_test: FAILED" : "m4_test: ok");
	return failures != 0;
}
