/* Baseline JPEG encoder (JFIF, YCbCr 4:2:0, the example tables of ITU-T
 * T.81 Annex K) for XRGB8888 images, one row of 16-pixel MCUs per
 * jpeg_step so a caller can spread the work. len is 0 if out was too
 * small. */
#ifndef JPEG_H
#define JPEG_H
#include <stdint.h>
#include <stdbool.h>

struct jpeg {
	const uint32_t *pix;
	uint32_t width, height, stride;
	uint8_t *out;
	uint32_t size, len, row, bits, nbits;
	int dc[3];
	bool overflow;
	float fy[64], fc[64];
};

void jpeg_begin(struct jpeg *j, const uint32_t *xrgb, uint32_t width, uint32_t height, uint32_t stride, uint8_t *out,
		uint32_t size, unsigned quality);
bool jpeg_step(struct jpeg *j);
#endif
