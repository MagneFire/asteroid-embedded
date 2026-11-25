/* LZ4 block format, as SquashFS stores it: sequences of a token, literals
 * and a match, the last sequence literals only. Every length and offset is
 * checked against both buffers. */
#include "lz4.h"

static int length(const uint8_t **p, const uint8_t *end, size_t *len)
{
	if (*len != 15)
		return 0;
	uint8_t b;
	do {
		if (*p == end)
			return LZ4_E_TRUNCATED;
		b = *(*p)++;
		*len += b;
	} while (b == 255);
	return 0;
}

int lz4_block_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len)
{
	const uint8_t *p = in, *end = in + in_len;
	size_t o = 0;
	*out_len = 0;
	if (!in_len)
		return LZ4_E_TRUNCATED;
	for (;;) {
		uint8_t token = *p++;
		size_t lit = token >> 4;
		int r = length(&p, end, &lit);
		if (r)
			return r;
		if (lit > (size_t)(end - p))
			return LZ4_E_TRUNCATED;
		if (lit > out_cap - o)
			return LZ4_E_OVERFLOW;
		for (size_t i = 0; i < lit; i++)
			out[o++] = p[i];
		p += lit;
		if (p == end)
			break;
		if (end - p < 2)
			return LZ4_E_TRUNCATED;
		size_t off = p[0] | (size_t)p[1] << 8;
		p += 2;
		if (!off || off > o)
			return LZ4_E_CORRUPT;
		size_t n = token & 15;
		if ((r = length(&p, end, &n)))
			return r;
		n += 4;
		if (n > out_cap - o)
			return LZ4_E_OVERFLOW;
		for (size_t i = 0; i < n; i++, o++)
			out[o] = out[o - off];
		if (p == end)
			return LZ4_E_TRUNCATED;
	}
	*out_len = o;
	return 0;
}
