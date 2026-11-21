#include <stdbool.h>
#include "display/rm69090_id.h"

uint8_t rm69090_byte(const uint8_t raw[2], enum rm69090_framing f)
{
	return f == RM69090_LATE ? (uint8_t)(raw[0] << 1 | raw[1] >> 7) : raw[0];
}

enum rm69090_framing rm69090_match(const uint8_t raw[3][2], const uint8_t id[3])
{
	uint32_t want = 0, got = 0, late = 0;
	for (unsigned i = 0; i < 3; i++) {
		want = want << 8 | id[i];
		got = got << 8 | raw[i][0];
		late = late << 8 | rm69090_byte(raw[i], RM69090_LATE);
	}
	if (!want)
		return RM69090_UNKNOWN;
	if (got == want)
		return RM69090_ON_TIME;
	if (late == want)
		return RM69090_LATE;
	/* Read a byte at a time, a late reply starts with the level the
	 * line was left at: the last bit of the reply before. The first
	 * has none before it. */
	if ((got & 0x7fffff) == want >> 1)
		return RM69090_LATE;
	return RM69090_UNKNOWN;
}

const char *rm69090_framing_name(enum rm69090_framing f)
{
	return f == RM69090_ON_TIME ? "on time" : f == RM69090_LATE ? "a bit late" : "unknown";
}
