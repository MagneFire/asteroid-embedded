#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdint.h>

struct fb {
	void *base;
	uint32_t width, height, stride, bpp;
};
extern struct fb fb;

#endif
