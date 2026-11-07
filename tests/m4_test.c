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

	/* Still and motion are always followed, the gesture only when wanted. */
	CHECK(m4_wake_mask(true) == 7);
	CHECK(m4_wake_mask(false) == 3);

	uint8_t le[4];
	m4_put_u16(M4_STILL_TIMEOUT_S, le);
	CHECK(le[0] == 0x58 && le[1] == 0x02);
	m4_put_u32(0x12345678, le);
	CHECK(m4_u32(le) == 0x12345678 && le[0] == 0x78);

	/* The pedometer block starts at activity; the count sits two bytes in. */
	uint8_t block[M4_PEDOMETER_BLOCK_SIZE] = { 1, 0, 0x78, 0x56, 0x34, 0x12 };
	CHECK(m4_block_steps(block) == 0x12345678);

	uint8_t magic[4] = { 0xef, 0xbe, 0xad, 0xde }, healthy[4] = { 0xff, 0xff, 0xff, 0xff }, zeros[4] = { 0 };
	CHECK(m4_panicked(magic));
	CHECK(!m4_panicked(healthy));
	CHECK(!m4_panicked(zeros));

	puts(failures ? "m4_test: FAILED" : "m4_test: ok");
	return failures != 0;
}
