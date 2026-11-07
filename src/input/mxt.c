/* Objects are 6 bytes in the table: type, address low and high, size - 1,
 * instances - 1, report ids per instance. Report ids count up from 1
 * through the objects in address order, which is not always table order.
 * A T9 message is report id, status, x and y high bytes, their low
 * nibbles, area and amplitude; a finger is down while detected, unless
 * released or suppressed. */
#include <string.h>
#include "mxt.h"

#define T9_DETECT	0x80
#define T9_PRESS	0x40
#define T9_RELEASE	0x20
#define T9_SUPPRESS	0x02
#define T9_WIDE_MIN	0x04

static unsigned address(const uint8_t *o)
{
	return (unsigned)(o[1] | o[2] << 8);
}

bool mxt_parse(uint8_t *table, unsigned n, struct mxt_map *m)
{
	memset(m, 0, sizeof *m);
	if (n > MXT_MAX_OBJECTS)
		return false;
	for (unsigned i = 1; i < n; i++)
		for (unsigned j = i; j && address(table + (j - 1) * MXT_OBJECT_SIZE) > address(table + j * MXT_OBJECT_SIZE); j--) {
			uint8_t t[MXT_OBJECT_SIZE], *a = table + (j - 1) * MXT_OBJECT_SIZE, *b = table + j * MXT_OBJECT_SIZE;
			memcpy(t, a, sizeof t);
			memcpy(a, b, sizeof t);
			memcpy(b, t, sizeof t);
		}
	unsigned id = 1;
	for (unsigned i = 0; i < n; i++) {
		const uint8_t *o = table + i * MXT_OBJECT_SIZE;
		unsigned ids = (unsigned)o[5] * (o[4] + 1u);
		if (o[0] == MXT_T5_MESSAGE) {
			m->msg_addr = (uint16_t)address(o);
			m->msg_size = o[3];
		} else if (o[0] == MXT_T9_TOUCH && ids) {
			m->t9_addr = (uint16_t)address(o);
			m->t9_size = (uint8_t)(o[3] + 1);
			m->t9_first_id = (uint8_t)id;
			m->t9_ids = (uint8_t)ids;
		} else if (o[0] == MXT_T42_SUPPRESS) {
			m->t42_addr = (uint16_t)address(o);
			m->t42_id = ids ? (uint8_t)id : 0;
		} else if (o[0] == MXT_T7_POWER) {
			m->t7_addr = (uint16_t)address(o);
		} else if (o[0] == MXT_T46_CTE) {
			m->t46_addr = (uint16_t)address(o);
		} else if (o[0] == MXT_T62_NOISE) {
			m->t62_addr = (uint16_t)address(o);
		}
		id += ids;
		if (id > 255)
			return false;
	}
	return m->msg_size >= 7 && m->t9_ids;
}

/* From the vendor's atmxt-r3_large.tdat, its enter and exit records. */
static const struct { uint8_t object, offset, asleep, awake; } power_writes[MXT_POWER_WRITES] = {
	{ MXT_T7_POWER, 0, 100, 16 },
	{ MXT_T7_POWER, 1, 100, 10 },
	{ MXT_T7_POWER, 2, 0, 10 },
	{ MXT_T7_POWER, 3, 0, 2 },
	{ MXT_T9_TOUCH, 7, 75, 85 },
	{ MXT_T46_CTE, 2, 8, 32 },
	{ MXT_T46_CTE, 3, 8, 32 },
	{ MXT_T62_NOISE, 24, 8, 100 },
	{ MXT_T42_SUPPRESS, 0, 65, 67 },
	{ MXT_T42_SUPPRESS, 2, 20, 50 },
	{ MXT_T42_SUPPRESS, 3, 20, 45 },
	{ MXT_T42_SUPPRESS, 4, 64, 0 },
};

bool mxt_power_write(const struct mxt_map *m, unsigned i, bool asleep, uint16_t *addr, uint8_t *val)
{
	uint16_t base = 0;
	switch (power_writes[i].object) {
	case MXT_T7_POWER: base = m->t7_addr; break;
	case MXT_T9_TOUCH: base = m->t9_addr; break;
	case MXT_T42_SUPPRESS: base = m->t42_addr; break;
	case MXT_T46_CTE: base = m->t46_addr; break;
	case MXT_T62_NOISE: base = m->t62_addr; break;
	}
	if (!base)
		return false;
	*addr = (uint16_t)(base + power_writes[i].offset);
	*val = asleep ? power_writes[i].asleep : power_writes[i].awake;
	return true;
}

bool mxt_palm(const struct mxt_map *m, const uint8_t *msg)
{
	return m->t42_id && msg[0] == m->t42_id && msg[1] & 1;
}

bool mxt_wide(uint8_t range_hi)
{
	return range_hi >= T9_WIDE_MIN;
}

bool mxt_decode(const struct mxt_map *m, const uint8_t *msg, bool wide_x, bool wide_y, struct mxt_touch *t)
{
	if (msg[0] < m->t9_first_id || msg[0] >= m->t9_first_id + m->t9_ids)
		return false;
	t->finger = (uint8_t)(msg[0] - m->t9_first_id);
	t->down = (msg[1] & T9_DETECT) && !(msg[1] & (T9_RELEASE | T9_SUPPRESS));
	/* A tap shorter than a scan comes as one press and release. */
	t->tap = (msg[1] & (T9_PRESS | T9_RELEASE | T9_SUPPRESS)) == (T9_PRESS | T9_RELEASE);
	t->x = (uint16_t)(msg[2] << 4 | msg[4] >> 4);
	t->y = (uint16_t)(msg[3] << 4 | (msg[4] & 0x0f));
	if (!wide_x)
		t->x >>= 2;
	if (!wide_y)
		t->y >>= 2;
	return true;
}
