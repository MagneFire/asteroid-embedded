#include <string.h>
#include "jpeg.h"

static const uint8_t zigzag[64] = {
	0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
	41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
	30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static const uint8_t q_luma[64] = {
	16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
	14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
	18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
	49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};

static const uint8_t q_chroma[64] = {
	17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
	99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

static const uint8_t dc_luma_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t dc_chroma_bits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t dc_values[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

static const uint8_t ac_luma_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t ac_luma_values[162] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
	0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
	0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
	0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
	0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
	0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
	0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
	0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

static const uint8_t ac_chroma_bits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t ac_chroma_values[162] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
	0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
	0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
	0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
	0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
	0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
	0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
	0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

static const float aan[8] = { 1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
			      1.0f, 0.785694958f, 0.541196100f, 0.275899379f };

struct huff {
	uint16_t code[256];
	uint8_t len[256];
};

static struct huff dc_luma, dc_chroma, ac_luma, ac_chroma;

static void huff_build(struct huff *h, const uint8_t bits[16], const uint8_t *values)
{
	unsigned code = 0, k = 0;
	for (unsigned l = 1; l <= 16; l++) {
		for (unsigned i = 0; i < bits[l - 1]; i++, k++) {
			h->code[values[k]] = (uint16_t)code++;
			h->len[values[k]] = (uint8_t)l;
		}
		code <<= 1;
	}
}

static void byte(struct jpeg *j, uint8_t b)
{
	if (j->len < j->size)
		j->out[j->len++] = b;
	else
		j->overflow = true;
}

static void bytes(struct jpeg *j, const uint8_t *p, unsigned n)
{
	while (n--)
		byte(j, *p++);
}

static void put_bits(struct jpeg *j, uint32_t v, unsigned n)
{
	j->bits = j->bits << n | (v & ((1u << n) - 1));
	j->nbits += n;
	while (j->nbits >= 8) {
		uint8_t b = (uint8_t)(j->bits >> (j->nbits - 8));
		byte(j, b);
		if (b == 0xff)
			byte(j, 0);
		j->nbits -= 8;
	}
}

static void put_code(struct jpeg *j, const struct huff *h, unsigned symbol)
{
	put_bits(j, h->code[symbol], h->len[symbol]);
}

static unsigned magnitude(int v, uint32_t *bits)
{
	unsigned a = (unsigned)(v < 0 ? -v : v), n = 0;
	while (a >> n)
		n++;
	*bits = (uint32_t)(v < 0 ? v - 1 : v);
	return n;
}

static void fdct8(float *d, unsigned step)
{
	float t0 = d[0] + d[7 * step], t7 = d[0] - d[7 * step];
	float t1 = d[step] + d[6 * step], t6 = d[step] - d[6 * step];
	float t2 = d[2 * step] + d[5 * step], t5 = d[2 * step] - d[5 * step];
	float t3 = d[3 * step] + d[4 * step], t4 = d[3 * step] - d[4 * step];
	float t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
	d[0] = t10 + t11;
	d[4 * step] = t10 - t11;
	float z1 = (t12 + t13) * 0.707106781f;
	d[2 * step] = t13 + z1;
	d[6 * step] = t13 - z1;
	t10 = t4 + t5;
	t11 = t5 + t6;
	t12 = t6 + t7;
	float z5 = (t10 - t12) * 0.382683433f;
	float z2 = 0.541196100f * t10 + z5, z4 = 1.306562965f * t12 + z5, z3 = t11 * 0.707106781f;
	float z11 = t7 + z3, z13 = t7 - z3;
	d[5 * step] = z13 + z2;
	d[3 * step] = z13 - z2;
	d[step] = z11 + z4;
	d[7 * step] = z11 - z4;
}

static void block(struct jpeg *j, float *d, const float *f, int *dc, const struct huff *hdc, const struct huff *hac)
{
	for (unsigned i = 0; i < 8; i++)
		fdct8(d + i * 8, 1);
	for (unsigned i = 0; i < 8; i++)
		fdct8(d + i, 8);
	int q[64];
	for (unsigned k = 0; k < 64; k++) {
		float v = d[zigzag[k]] * f[zigzag[k]];
		q[k] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
	}
	uint32_t bits;
	unsigned n = magnitude(q[0] - *dc, &bits);
	*dc = q[0];
	put_code(j, hdc, n);
	put_bits(j, bits, n);
	unsigned run = 0;
	for (unsigned k = 1; k < 64; k++) {
		if (!q[k]) {
			run++;
			continue;
		}
		for (; run > 15; run -= 16)
			put_code(j, hac, 0xf0);
		n = magnitude(q[k], &bits);
		put_code(j, hac, run << 4 | n);
		put_bits(j, bits, n);
		run = 0;
	}
	if (run)
		put_code(j, hac, 0);
}

static void scale_table(uint8_t *out, float *f, const uint8_t *base, unsigned quality)
{
	unsigned s = quality < 50 ? 5000 / quality : 200 - quality * 2;
	for (unsigned i = 0; i < 64; i++) {
		unsigned v = (base[i] * s + 50) / 100;
		v = v < 1 ? 1 : v > 255 ? 255 : v;
		f[i] = 1.0f / ((float)v * aan[i / 8] * aan[i % 8] * 8.0f);
	}
	for (unsigned k = 0; k < 64; k++) {
		unsigned v = (base[zigzag[k]] * s + 50) / 100;
		out[k] = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v);
	}
}

static void dht(struct jpeg *j, uint8_t class_id, const uint8_t bits[16], const uint8_t *values, unsigned n)
{
	byte(j, class_id);
	bytes(j, bits, 16);
	bytes(j, values, n);
}

void jpeg_begin(struct jpeg *j, const uint32_t *xrgb, uint32_t width, uint32_t height, uint32_t stride, uint8_t *out,
		uint32_t size, unsigned quality)
{
	if (!dc_luma.len[0]) {
		huff_build(&dc_luma, dc_luma_bits, dc_values);
		huff_build(&dc_chroma, dc_chroma_bits, dc_values);
		huff_build(&ac_luma, ac_luma_bits, ac_luma_values);
		huff_build(&ac_chroma, ac_chroma_bits, ac_chroma_values);
	}
	memset(j, 0, sizeof *j);
	j->pix = xrgb;
	j->width = width;
	j->height = height;
	j->stride = stride;
	j->out = out;
	j->size = size;
	quality = quality < 1 ? 1 : quality > 100 ? 100 : quality;
	uint8_t qy[64], qc[64];
	scale_table(qy, j->fy, q_luma, quality);
	scale_table(qc, j->fc, q_chroma, quality);
	static const uint8_t head[] = { 0xff, 0xd8, 0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
					0xff, 0xdb, 0, 132 };
	bytes(j, head, sizeof head);
	byte(j, 0);
	bytes(j, qy, 64);
	byte(j, 1);
	bytes(j, qc, 64);
	const uint8_t sof[] = { 0xff, 0xc0, 0, 17, 8, (uint8_t)(height >> 8), (uint8_t)height, (uint8_t)(width >> 8),
				(uint8_t)width, 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1, 0xff, 0xc4, 0x01, 0xa2 };
	bytes(j, sof, sizeof sof);
	dht(j, 0x00, dc_luma_bits, dc_values, sizeof dc_values);
	dht(j, 0x10, ac_luma_bits, ac_luma_values, sizeof ac_luma_values);
	dht(j, 0x01, dc_chroma_bits, dc_values, sizeof dc_values);
	dht(j, 0x11, ac_chroma_bits, ac_chroma_values, sizeof ac_chroma_values);
	static const uint8_t sos[] = { 0xff, 0xda, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0 };
	bytes(j, sos, sizeof sos);
}

bool jpeg_step(struct jpeg *j)
{
	if (j->row * 16 >= j->height)
		return true;
	float y[4][64], cb[64], cr[64];
	for (uint32_t mx = 0; mx * 16 < j->width; mx++) {
		memset(cb, 0, sizeof cb);
		memset(cr, 0, sizeof cr);
		for (unsigned py = 0; py < 16; py++) {
			uint32_t sy = j->row * 16 + py;
			const uint32_t *line = j->pix + (sy < j->height ? sy : j->height - 1) * j->stride;
			for (unsigned px = 0; px < 16; px++) {
				uint32_t sx = mx * 16 + px;
				uint32_t p = line[sx < j->width ? sx : j->width - 1];
				float r = (float)(p >> 16 & 0xff), g = (float)(p >> 8 & 0xff), b = (float)(p & 0xff);
				y[(py / 8) * 2 + px / 8][(py % 8) * 8 + px % 8] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
				unsigned c = (py / 2) * 8 + px / 2;
				cb[c] += 0.25f * (-0.168736f * r - 0.331264f * g + 0.5f * b);
				cr[c] += 0.25f * (0.5f * r - 0.418688f * g - 0.081312f * b);
			}
		}
		for (unsigned i = 0; i < 4; i++)
			block(j, y[i], j->fy, &j->dc[0], &dc_luma, &ac_luma);
		block(j, cb, j->fc, &j->dc[1], &dc_chroma, &ac_chroma);
		block(j, cr, j->fc, &j->dc[2], &dc_chroma, &ac_chroma);
	}
	if (++j->row * 16 < j->height)
		return false;
	if (j->nbits)
		put_bits(j, 0x7f, 8 - j->nbits);
	byte(j, 0xff);
	byte(j, 0xd9);
	if (j->overflow)
		j->len = 0;
	return true;
}
