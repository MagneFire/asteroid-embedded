/* maXTouch object table parsing and message decoding. */
#include <stdio.h>
#include <string.h>
#include "input/mxt.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

int main(void)
{
	/* type, addr lo, hi, size - 1, instances - 1, ids: out of address
	 * order, so T6's id comes before T9's. */
	uint8_t table[] = {
		9, 0x20, 0x01, 34, 0, 10,
		6, 0x10, 0x01, 5, 0, 1,
		5, 0x00, 0x01, 8, 0, 0,
		42, 0x60, 0x01, 9, 0, 1,
	};
	struct mxt_map m;
	CHECK(mxt_parse(table, 4, &m));
	CHECK(m.msg_addr == 0x100 && m.msg_size == 8);
	CHECK(m.t9_addr == 0x120 && m.t9_size == 35);
	CHECK(m.t9_first_id == 2 && m.t9_ids == 10);

	/* Touch suppression reports a palm, on its own id only. */
	CHECK(m.t42_id == 12);
	CHECK(mxt_palm(&m, (const uint8_t[]){ 12, 1 }));
	CHECK(!mxt_palm(&m, (const uint8_t[]){ 12, 0 }));
	CHECK(!mxt_palm(&m, (const uint8_t[]){ 2, 1 }));

	/* The low power settings, with a missing object left out. */
	uint16_t addr = 0;
	uint8_t val = 0;
	CHECK(!mxt_power_write(&m, 0, true, &addr, &val));
	CHECK(mxt_power_write(&m, 4, true, &addr, &val));
	CHECK(addr == 0x127 && val == 75);
	CHECK(mxt_power_write(&m, 4, false, &addr, &val));
	CHECK(addr == 0x127 && val == 85);
	CHECK(mxt_power_write(&m, 8, true, &addr, &val));
	CHECK(addr == 0x160 && val == 65);

	/* Without a touch object, or too many. */
	uint8_t no_t9[] = { 5, 0, 1, 8, 0, 0 };
	CHECK(!mxt_parse(no_t9, 1, &m));
	CHECK(!mxt_parse(table, MXT_MAX_OBJECTS + 1, &m));

	/* Instances multiply the ids. */
	uint8_t two[] = { 5, 0, 1, 8, 0, 0, 15, 0x10, 1, 3, 1, 2, 9, 0x20, 1, 34, 0, 10 };
	CHECK(mxt_parse(two, 3, &m) && m.t9_first_id == 5);

	CHECK(mxt_parse(table, 4, &m));
	struct mxt_touch t;
	uint8_t down[] = { 2, 0xc0, 0x12, 0x34, 0x56, 0x10, 0x20, 0 };
	CHECK(mxt_decode(&m, down, true, true, &t));
	CHECK(t.finger == 0 && t.down && t.x == 0x125 && t.y == 0x346);
	CHECK(mxt_decode(&m, down, false, false, &t));
	CHECK(t.x == 0x125 >> 2 && t.y == 0x346 >> 2);

	uint8_t second[] = { 3, 0x90, 0, 0, 0, 0, 0, 0 };
	CHECK(mxt_decode(&m, second, true, true, &t) && t.finger == 1);

	uint8_t up[] = { 2, 0x20, 0, 0, 0, 0, 0, 0 };
	CHECK(mxt_decode(&m, up, true, true, &t) && !t.down);
	uint8_t suppressed[] = { 2, 0x02, 0, 0, 0, 0, 0, 0 };
	CHECK(mxt_decode(&m, suppressed, true, true, &t) && !t.down);

	/* Down needs DETECT; a move on its own says nothing is there. */
	uint8_t moved[] = { 2, 0x90, 0, 0, 0, 0, 0, 0 };
	CHECK(mxt_decode(&m, moved, true, true, &t) && t.down);
	uint8_t undetected[] = { 2, 0x10, 0, 0, 0, 0, 0, 0 };
	CHECK(mxt_decode(&m, undetected, true, true, &t) && !t.down);
	uint8_t no_status[] = { 2, 0x00, 0, 0, 0, 0, 0, 0 };
	CHECK(mxt_decode(&m, no_status, true, true, &t) && !t.down);
	/* A tap within one scan: press and release together. */
	uint8_t tap[] = { 2, 0x60, 0x12, 0x34, 0x56, 0, 0, 0 };
	CHECK(mxt_decode(&m, tap, true, true, &t) && !t.down && t.tap && t.x == 0x125);
	CHECK(mxt_decode(&m, up, true, true, &t) && !t.tap);
	uint8_t tap_suppressed[] = { 2, 0x62, 0, 0, 0, 0, 0, 0 };
	CHECK(mxt_decode(&m, tap_suppressed, true, true, &t) && !t.tap);
	CHECK(mxt_decode(&m, down, true, true, &t) && !t.tap);
	uint8_t detected_up[] = { 2, 0xa0, 0, 0, 0, 0, 0, 0 };
	CHECK(mxt_decode(&m, detected_up, true, true, &t) && !t.down);
	uint8_t detected_suppressed[] = { 2, 0x82, 0, 0, 0, 0, 0, 0 };
	CHECK(mxt_decode(&m, detected_suppressed, true, true, &t) && !t.down);

	/* Other objects' messages are not touches. */
	uint8_t other[] = { 1, 0, 0, 0, 0, 0, 0, 0 };
	CHECK(!mxt_decode(&m, other, true, true, &t));
	uint8_t past[] = { 12, 0xc0, 0, 0, 0, 0, 0, 0 };
	CHECK(!mxt_decode(&m, past, true, true, &t));

	CHECK(mxt_wide(0x04) && mxt_wide(0x0f) && !mxt_wide(0x03));

	puts(failures ? "mxt_test: FAILED" : "mxt_test: ok");
	return failures != 0;
}
