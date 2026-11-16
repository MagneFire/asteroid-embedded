#include <string.h>
#include "proto.h"

#define VARINT		0
#define FIXED64		1
#define BYTES		2
#define FIXED32		5
#define BATTERY_CMD	17
#define RESET_PIN	(1u << 0)
#define RESET_POWER_ON	(1u << 1)
#define RESET_BROWN_OUT	(1u << 2)
#define RESET_DEBUGGER	(1u << 5)
#define RESET_WATCHDOG	(1u << 6)

struct reader {
	const uint8_t *p, *end;
	bool bad;
};

struct field {
	unsigned num;
	uint64_t v;
	const uint8_t *bytes;
	unsigned len;
};

struct writer {
	uint8_t *p;
	unsigned n, cap;
	bool full;
};

uint32_t proto_zigzag(int32_t v) { return (uint32_t)v << 1 ^ (uint32_t)(v >> 31); }
int32_t proto_unzigzag(uint32_t v) { return (int32_t)(v >> 1 ^ (0u - (v & 1))); }

static bool varint(struct reader *r, uint64_t *v)
{
	*v = 0;
	for (unsigned shift = 0; shift < 64 && r->p < r->end; shift += 7) {
		uint8_t b = *r->p++;
		*v |= (uint64_t)(b & 0x7f) << shift;
		if (!(b & 0x80))
			return true;
	}
	return false;
}

static bool read_field(struct reader *r, struct field *f);

/* The next field, false at the end or at something malformed, which
 * also sets bad. */
static bool next(struct reader *r, struct field *f)
{
	if (r->p >= r->end)
		return false;
	if (read_field(r, f))
		return true;
	r->bad = true;
	return false;
}

static bool read_field(struct reader *r, struct field *f)
{
	uint64_t key;
	if (!varint(r, &key))
		return false;
	f->num = (unsigned)(key >> 3);
	f->v = 0;
	f->bytes = NULL;
	f->len = 0;
	switch (key & 7) {
	case VARINT:
		return varint(r, &f->v);
	case BYTES:
		if (!varint(r, &f->v) || f->v > (uint64_t)(r->end - r->p))
			return false;
		f->bytes = r->p;
		f->len = (unsigned)f->v;
		r->p += f->len;
		return true;
	case FIXED64:
	case FIXED32: {
		unsigned n = (key & 7) == FIXED64 ? 8 : 4;
		if ((unsigned)(r->end - r->p) < n)
			return false;
		r->p += n;
		return true;
	}
	default:
		return false;
	}
}

static struct reader reader(const uint8_t *p, unsigned n) { return (struct reader){ p, p + n, false }; }

static bool time_get(const uint8_t *p, unsigned n, unsigned time_field, struct proto_time *t)
{
	struct reader r = reader(p, n);
	struct field f;
	while (next(&r, &f))
		if (f.num == time_field && !f.bytes)
			t->time = (int32_t)(uint32_t)f.v;
		else if (f.num == 3 && !f.bytes)
			t->zone_min = proto_unzigzag((uint32_t)f.v);
	return !r.bad;
}

int proto_mode_of(unsigned id)
{
	return id == PROTO_NORMAL ? PROTO_MODE_NORMAL : id == PROTO_WRISTBAND ? PROTO_MODE_WRISTBAND :
	       id == PROTO_POWER_OFF ? PROTO_MODE_POWER_OFF : -1;
}

bool proto_work_mode(const uint8_t *p, unsigned n, unsigned *mode, struct proto_time *t)
{
	struct reader r = reader(p, n);
	struct field f;
	bool ok = true;
	*mode = 0;
	*t = (struct proto_time){ 0, 0 };
	while (ok && next(&r, &f))
		if (f.num == 1 && !f.bytes)
			*mode = (unsigned)f.v;
		else if (f.num == 2 && f.bytes)
			ok = time_get(f.bytes, f.len, 1, t);
	return ok && !r.bad;
}

int proto_battery(const uint8_t *p, unsigned n)
{
	struct reader r = reader(p, n);
	struct field f;
	uint64_t cmd = 0;
	int capacity = -1;
	bool ok = true;
	while (ok && next(&r, &f))
		if (f.num == 1 && !f.bytes)
			cmd = f.v;
		else if (f.num == 15 && f.bytes) {
			struct reader b = reader(f.bytes, f.len);
			struct field g;
			capacity = 0;
			while (next(&b, &g))
				if (g.num == 2 && !g.bytes)
					capacity = g.v > 100 ? 100 : (int)g.v;
			ok = !b.bad;
		}
	return ok && !r.bad && cmd == BATTERY_CMD ? capacity : -1;
}

bool proto_boot(const uint8_t *p, unsigned n, struct proto_time *t)
{
	struct reader r = reader(p, n);
	struct field f;
	bool ok = true;
	*t = (struct proto_time){ 0, 0 };
	while (ok && next(&r, &f))
		if (f.num == 3 && f.bytes)
			ok = time_get(f.bytes, f.len, 1, t);
	return ok && !r.bad;
}

static void put_byte(struct writer *w, uint8_t b)
{
	if (w->n < w->cap)
		w->p[w->n++] = b;
	else
		w->full = true;
}

static void put_varint(struct writer *w, uint64_t v)
{
	do {
		put_byte(w, (uint8_t)((v & 0x7f) | (v > 0x7f ? 0x80 : 0)));
		v >>= 7;
	} while (v);
}

static void put_int(struct writer *w, unsigned num, int64_t v)
{
	if (!v)
		return;
	put_varint(w, (uint64_t)num << 3 | VARINT);
	put_varint(w, (uint64_t)v);
}

static void put_bytes(struct writer *w, unsigned num, const uint8_t *p, unsigned n)
{
	put_varint(w, (uint64_t)num << 3 | BYTES);
	put_varint(w, n);
	for (unsigned i = 0; i < n; i++)
		put_byte(w, p[i]);
}

int proto_reply_put(uint8_t *out, unsigned cap, const struct proto_reply *r)
{
	struct writer w = { out, 0, cap, false };
	put_int(&w, 1, r->mode);
	put_int(&w, 3, r->last_mode);
	if (r->time.time) {
		uint8_t t[16];
		struct writer tw = { t, 0, sizeof t, false };
		put_int(&tw, 2, r->time.time);
		put_int(&tw, 3, proto_zigzag(r->time.zone_min));
		put_bytes(&w, 4, t, tw.n);
	}
	put_int(&w, 5, r->reboot);
	return w.full ? -1 : (int)w.n;
}

bool proto_powered_up(uint32_t st) { return st & (RESET_POWER_ON | RESET_BROWN_OUT); }

unsigned proto_reboot_reason(uint32_t st, bool faulted)
{
	if (st & RESET_DEBUGGER)
		return PROTO_REBOOT_DEBUG;
	if ((st & RESET_WATCHDOG) || faulted)
		return PROTO_REBOOT_FAULT;
	if ((st & RESET_PIN) && !(st & (RESET_POWER_ON | RESET_BROWN_OUT)))
		return PROTO_REBOOT_PIN;
	return PROTO_REBOOT_OTHER;
}
