/* The RM69090's ID and power mode replies as read on time and a clock
 * late, for both OPPO Watch panels and one that is neither. A late
 * reply is played as the line does it: the level left by the reply
 * before, then the byte, then its last bit held. */
#include <stdio.h>
#include <string.h>
#include "display/rm69090_id.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const uint8_t id_46[3] = { 0x09, 0x80, 0x01 }, id_41[3] = { 0x01, 0x80, 0x01 };

static void on_time(const uint8_t id[3], uint8_t raw[3][2])
{
	for (unsigned i = 0; i < 3; i++) {
		raw[i][0] = id[i];
		raw[i][1] = id[i];
	}
}

static void late(const uint8_t id[3], uint8_t raw[3][2])
{
	unsigned line = 0;
	for (unsigned i = 0; i < 3; i++) {
		raw[i][0] = (uint8_t)(line << 7 | id[i] >> 1);
		line = id[i] & 1;
		raw[i][1] = (uint8_t)(line ? 0xff : 0x00);
	}
}

/* The first byte of each reply only, as the firmware read them. */
static void late_one_byte(const uint8_t id[3], uint8_t raw[3][2])
{
	late(id, raw);
	for (unsigned i = 0; i < 3; i++)
		raw[i][1] = 0;
}

int main(void)
{
	uint8_t raw[3][2];

	on_time(id_46, raw);
	CHECK(rm69090_match(raw, id_46) == RM69090_ON_TIME);
	CHECK(rm69090_match(raw, id_41) == RM69090_UNKNOWN);
	on_time(id_41, raw);
	CHECK(rm69090_match(raw, id_41) == RM69090_ON_TIME);
	CHECK(rm69090_match(raw, id_46) == RM69090_UNKNOWN);

	late(id_46, raw);
	CHECK(raw[0][0] == 0x04 && raw[1][0] == 0xc0 && raw[2][0] == 0x00);
	CHECK(rm69090_match(raw, id_46) == RM69090_LATE);
	CHECK(rm69090_match(raw, id_41) == RM69090_UNKNOWN);
	for (unsigned i = 0; i < 3; i++)
		CHECK(rm69090_byte(raw[i], RM69090_LATE) == id_46[i]);
	late(id_41, raw);
	CHECK(raw[0][0] == 0x00 && raw[1][0] == 0xc0 && raw[2][0] == 0x00);
	CHECK(rm69090_match(raw, id_41) == RM69090_LATE);
	CHECK(rm69090_match(raw, id_46) == RM69090_UNKNOWN);

	/* The 46 mm watch's boot log: 04 c0 00. */
	late_one_byte(id_46, raw);
	CHECK(rm69090_match(raw, id_46) == RM69090_LATE);
	CHECK(rm69090_match(raw, id_41) == RM69090_UNKNOWN);
	late_one_byte(id_41, raw);
	CHECK(rm69090_match(raw, id_41) == RM69090_LATE);
	CHECK(rm69090_match(raw, id_46) == RM69090_UNKNOWN);

	/* Reads that did not work, and a panel of neither watch. */
	memset(raw, 0, sizeof raw);
	CHECK(rm69090_match(raw, id_46) == RM69090_UNKNOWN && rm69090_match(raw, id_41) == RM69090_UNKNOWN);
	memset(raw, 0xff, sizeof raw);
	CHECK(rm69090_match(raw, id_46) == RM69090_UNKNOWN && rm69090_match(raw, id_41) == RM69090_UNKNOWN);
	on_time((const uint8_t[]){ 0x00, 0x80, 0x00 }, raw);
	CHECK(rm69090_match(raw, id_46) == RM69090_UNKNOWN && rm69090_match(raw, id_41) == RM69090_UNKNOWN);
	CHECK(rm69090_match(raw, (const uint8_t[]){ 0, 0, 0 }) == RM69090_UNKNOWN);

	/* Power mode B4 (booster, partial, out of sleep, display on) after
	 * DC's 01: read late, sleep-out (bit 4) would come from the partial
	 * bit and display-on (bit 2) from normal mode, which is off. */
	uint8_t mode[2] = { 0x80 | 0xb4 >> 1, 0x00 };
	CHECK((mode[0] & 0x14) != 0x14);
	CHECK(rm69090_byte(mode, RM69090_LATE) == 0xb4);
	mode[0] = mode[1] = 0xb4;
	CHECK(rm69090_byte(mode, RM69090_ON_TIME) == 0xb4);
	CHECK(rm69090_byte(mode, RM69090_UNKNOWN) == 0xb4);

	if (failures)
		printf("rm69090_test: %d failures\n", failures);
	else
		printf("rm69090_test: ok\n");
	return failures != 0;
}
