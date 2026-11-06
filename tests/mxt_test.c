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

	/* Other objects' messages are not touches. */
	uint8_t other[] = { 1, 0, 0, 0, 0, 0, 0, 0 };
	CHECK(!mxt_decode(&m, other, true, true, &t));
	uint8_t past[] = { 12, 0xc0, 0, 0, 0, 0, 0, 0 };
	CHECK(!mxt_decode(&m, past, true, true, &t));

	CHECK(mxt_wide(0x04) && mxt_wide(0x0f) && !mxt_wide(0x03));

	puts(failures ? "mxt_test: FAILED" : "mxt_test: ok");
	return failures != 0;
}
