/* Decompression of zlib streams (RFC 1950) holding deflate data (RFC 1951):
 * stored, fixed and dynamic Huffman blocks. The caller gives the whole
 * output buffer, which doubles as the back-reference window, so nothing
 * is allocated. Codes are decoded through a 10-bit lookup table with a
 * bit-by-bit canonical walk for the longer ones. Input is untrusted: a
 * stream that runs out, names a distance before the start of the output,
 * has an over-subscribed or incomplete code set, or would write past the
 * buffer is refused, and every loop consumes input or output. */
#include <stdbool.h>
#include <string.h>
#include "inflate.h"

#define MAX_BITS	15
#define FAST_BITS	10
#define FAST_SIZE	(1u << FAST_BITS)
#define MAX_LIT		288
#define MAX_DIST	30

struct huff {
	uint16_t count[MAX_BITS + 1];
	uint16_t sym[MAX_LIT];
	uint16_t fast[FAST_SIZE];
};

struct bits {
	const uint8_t *p, *end;
	uint32_t buf;
	unsigned cnt, pad;
};

struct state {
	struct bits br;
	uint8_t *out;
	size_t cap, pos;
	struct huff lit, dist;
};

static const uint16_t len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
	6145, 8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t clen_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

const char *inflate_strerror(int err)
{
	switch (err) {
	case INFLATE_E_HEADER: return "bad zlib header";
	case INFLATE_E_TRUNCATED: return "truncated stream";
	case INFLATE_E_CORRUPT: return "corrupt stream";
	case INFLATE_E_OVERFLOW: return "output buffer too small";
	case INFLATE_E_ADLER: return "checksum mismatch";
	default: return err ? "error" : "ok";
	}
}

uint32_t adler32(uint32_t adler, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	uint32_t a = adler & 0xffff, b = adler >> 16;
	while (len) {
		size_t n = len < 5552 ? len : 5552;
		len -= n;
		while (n--) {
			a += *p++;
			b += a;
		}
		a %= 65521;
		b %= 65521;
	}
	return b << 16 | a;
}

static void refill(struct bits *br)
{
	while (br->cnt <= 24) {
		if (br->p < br->end)
			br->buf |= (uint32_t)*br->p++ << br->cnt;
		else
			br->pad++;
		br->cnt += 8;
	}
}

/* Bits past the end of the input read as zero; using one is an error. */
static bool overrun(const struct bits *br)
{
	return br->pad * 8 > br->cnt;
}

static void drop(struct bits *br, unsigned n)
{
	br->buf >>= n;
	br->cnt -= n;
}

static int get_bits(struct bits *br, unsigned n, uint32_t *v)
{
	if (!n) {
		*v = 0;
		return 0;
	}
	refill(br);
	*v = br->buf & ((1u << n) - 1);
	drop(br, n);
	return overrun(br) ? INFLATE_E_TRUNCATED : 0;
}

static unsigned reverse(unsigned code, unsigned len)
{
	unsigned r = 0;
	while (len--) {
		r = r << 1 | (code & 1);
		code >>= 1;
	}
	return r;
}

/* An incomplete code is accepted only as the single one-bit code a stream
 * with one distance symbol uses, or as an empty distance code. */
static int build(struct huff *h, const uint8_t *lens, unsigned n, bool is_dist)
{
	unsigned total = 0;
	memset(h, 0, sizeof *h);
	for (unsigned i = 0; i < n; i++)
		h->count[lens[i]]++;
	h->count[0] = 0;
	int left = 1;
	for (unsigned l = 1; l <= MAX_BITS; l++) {
		left = (left << 1) - h->count[l];
		if (left < 0)
			return INFLATE_E_CORRUPT;
		total += h->count[l];
	}
	if (left > 0 && !(is_dist && (total == 0 || (total == 1 && h->count[1] == 1))))
		return INFLATE_E_CORRUPT;
	uint16_t offs[MAX_BITS + 2] = { 0 };
	for (unsigned l = 1; l <= MAX_BITS; l++)
		offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
	for (unsigned i = 0; i < n; i++)
		if (lens[i])
			h->sym[offs[lens[i]]++] = (uint16_t)i;
	unsigned code = 0, idx = 0;
	for (unsigned l = 1; l <= MAX_BITS; l++) {
		for (unsigned k = 0; k < h->count[l]; k++, code++, idx++) {
			if (l > FAST_BITS)
				continue;
			unsigned r = reverse(code, l);
			for (unsigned j = r; j < FAST_SIZE; j += 1u << l)
				h->fast[j] = (uint16_t)(h->sym[idx] << 4 | l);
		}
		code <<= 1;
	}
	return 0;
}

static int decode(struct bits *br, const struct huff *h, unsigned *sym)
{
	refill(br);
	unsigned e = h->fast[br->buf & (FAST_SIZE - 1)];
	if (e) {
		drop(br, e & 15);
		*sym = e >> 4;
		return overrun(br) ? INFLATE_E_TRUNCATED : 0;
	}
	int code = 0, first = 0, index = 0;
	for (unsigned l = 1; l <= MAX_BITS; l++) {
		code |= (int)((br->buf >> (l - 1)) & 1);
		int count = h->count[l];
		if (code - count < first) {
			drop(br, l);
			*sym = h->sym[index + (code - first)];
			return overrun(br) ? INFLATE_E_TRUNCATED : 0;
		}
		index += count;
		first = (first + count) << 1;
		code <<= 1;
	}
	return INFLATE_E_CORRUPT;
}

static int stored_block(struct state *s)
{
	struct bits *br = &s->br;
	uint32_t len, nlen;
	drop(br, br->cnt & 7);
	int r = get_bits(br, 16, &len);
	if (!r)
		r = get_bits(br, 16, &nlen);
	if (r)
		return r;
	if ((len ^ nlen) != 0xffff)
		return INFLATE_E_CORRUPT;
	if (len > s->cap - s->pos)
		return INFLATE_E_OVERFLOW;
	while (len && br->cnt) {
		s->out[s->pos++] = (uint8_t)br->buf;
		drop(br, 8);
		len--;
		if (overrun(br))
			return INFLATE_E_TRUNCATED;
	}
	if (len > (size_t)(br->end - br->p))
		return INFLATE_E_TRUNCATED;
	memcpy(s->out + s->pos, br->p, len);
	br->p += len;
	s->pos += len;
	return 0;
}

static int codes(struct state *s)
{
	struct bits *br = &s->br;
	for (;;) {
		unsigned sym;
		int r = decode(br, &s->lit, &sym);
		if (r)
			return r;
		if (sym < 256) {
			if (s->pos == s->cap)
				return INFLATE_E_OVERFLOW;
			s->out[s->pos++] = (uint8_t)sym;
			continue;
		}
		if (sym == 256)
			return 0;
		sym -= 257;
		if (sym >= 29)
			return INFLATE_E_CORRUPT;
		uint32_t extra;
		if ((r = get_bits(br, len_extra[sym], &extra)))
			return r;
		size_t len = len_base[sym] + extra;
		if ((r = decode(br, &s->dist, &sym)))
			return r;
		if (sym >= MAX_DIST)
			return INFLATE_E_CORRUPT;
		if ((r = get_bits(br, dist_extra[sym], &extra)))
			return r;
		size_t dist = dist_base[sym] + extra;
		if (dist > s->pos)
			return INFLATE_E_CORRUPT;
		if (len > s->cap - s->pos)
			return INFLATE_E_OVERFLOW;
		const uint8_t *from = s->out + s->pos - dist;
		uint8_t *to = s->out + s->pos;
		s->pos += len;
		while (len--)
			*to++ = *from++;
	}
}

static int fixed_block(struct state *s)
{
	uint8_t lens[MAX_LIT];
	unsigned i = 0;
	for (; i < 144; i++)
		lens[i] = 8;
	for (; i < 256; i++)
		lens[i] = 9;
	for (; i < 280; i++)
		lens[i] = 7;
	for (; i < MAX_LIT; i++)
		lens[i] = 8;
	int r = build(&s->lit, lens, MAX_LIT, false);
	if (r)
		return r;
	memset(lens, 5, 32);
	if ((r = build(&s->dist, lens, 32, true)))
		return r;
	return codes(s);
}

static int read_lengths(struct state *s, const struct huff *clen, uint8_t *lens, unsigned total)
{
	unsigned i = 0;
	while (i < total) {
		unsigned sym;
		uint32_t v;
		int r = decode(&s->br, clen, &sym);
		if (r)
			return r;
		if (sym < 16) {
			lens[i++] = (uint8_t)sym;
			continue;
		}
		unsigned rep, fill = 0;
		if (sym == 16) {
			if (!i)
				return INFLATE_E_CORRUPT;
			fill = lens[i - 1];
			r = get_bits(&s->br, 2, &v);
			rep = 3 + v;
		} else if (sym == 17) {
			r = get_bits(&s->br, 3, &v);
			rep = 3 + v;
		} else {
			r = get_bits(&s->br, 7, &v);
			rep = 11 + v;
		}
		if (r)
			return r;
		if (rep > total - i)
			return INFLATE_E_CORRUPT;
		while (rep--)
			lens[i++] = (uint8_t)fill;
	}
	return 0;
}

static int dynamic_block(struct state *s)
{
	uint32_t nlit, ndist, nclen;
	int r = get_bits(&s->br, 5, &nlit);
	if (!r)
		r = get_bits(&s->br, 5, &ndist);
	if (!r)
		r = get_bits(&s->br, 4, &nclen);
	if (r)
		return r;
	nlit += 257;
	ndist += 1;
	nclen += 4;
	if (nlit > 286 || ndist > MAX_DIST)
		return INFLATE_E_CORRUPT;
	uint8_t lens[286 + MAX_DIST] = { 0 };
	for (unsigned i = 0; i < nclen; i++) {
		uint32_t v;
		if ((r = get_bits(&s->br, 3, &v)))
			return r;
		lens[clen_order[i]] = (uint8_t)v;
	}
	if ((r = build(&s->lit, lens, 19, false)))
		return r;
	uint8_t all[286 + MAX_DIST];
	if ((r = read_lengths(s, &s->lit, all, nlit + ndist)))
		return r;
	if (!all[256])
		return INFLATE_E_CORRUPT;
	if ((r = build(&s->dist, all + nlit, ndist, true)))
		return r;
	if ((r = build(&s->lit, all, nlit, false)))
		return r;
	return codes(s);
}

int deflate_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len, size_t *in_used)
{
	struct state s;
	memset(&s.br, 0, sizeof s.br);
	s.br.p = in;
	s.br.end = in + in_len;
	s.out = out;
	s.cap = out_cap;
	s.pos = 0;
	uint32_t last = 0;
	while (!last) {
		uint32_t type;
		int r = get_bits(&s.br, 1, &last);
		if (!r)
			r = get_bits(&s.br, 2, &type);
		if (r)
			return r;
		if (type == 0)
			r = stored_block(&s);
		else if (type == 1)
			r = fixed_block(&s);
		else if (type == 2)
			r = dynamic_block(&s);
		else
			r = INFLATE_E_CORRUPT;
		if (r)
			return r;
	}
	*out_len = s.pos;
	if (in_used)
		*in_used = (size_t)(s.br.p - in) - (s.br.cnt / 8 - s.br.pad);
	return 0;
}

int zlib_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (in_len < 2)
		return INFLATE_E_TRUNCATED;
	if ((in[0] & 15) != 8 || (in[0] >> 4) > 7 || ((in[0] << 8 | in[1]) % 31) || (in[1] & 0x20))
		return INFLATE_E_HEADER;
	size_t used, n;
	int r = deflate_decode(in + 2, in_len - 2, out, out_cap, &n, &used);
	if (r)
		return r;
	used += 2;
	if (in_len - used < 4)
		return INFLATE_E_TRUNCATED;
	const uint8_t *a = in + used;
	uint32_t want = (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
	if (adler32(1, out, n) != want)
		return INFLATE_E_ADLER;
	*out_len = n;
	return 0;
}
