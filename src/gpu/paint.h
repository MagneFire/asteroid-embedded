/* Drawing the UI's rectangles, glyphs and images into a layer on the
 * GPU, as the UI toolkit walks its items. paint_begin starts on a layer
 * of stride_px * height premultiplied RGBA pixels, or returns false
 * while the GPU is off; pixel coordinates are the layer's, clip is x0,
 * y0, x1, y1 and colours are premultiplied 0xAABBGGRR. paint_rect fills
 * a rectangle, blended or (copy) replacing what is there. paint_rounded
 * fills a rectangle with rounded corners (radii top left, top right,
 * bottom right, bottom left) and a border of the given width in the edge
 * colour, anti-aliased, or returns false for a radius it cannot draw.
 * paint_texture draws width * height texels at data (a PAINT_* format,
 * stride bytes per row) scaled to the destination; tint multiplies every
 * texel, or with colorize only the texel's alpha is used. Texels are
 * copied into atlases on first use and found again by their address, so
 * they must not change. paint_cpu says the CPU has written rows y0 to y1
 * of the layer since the last call; paint_sync finishes the GPU's work
 * so the CPU can read and write the layer. paint_end ends the layer
 * without waiting for the GPU; paint_finish waits for all layers ended
 * so far, after which the CPU may read them. */
#ifndef GPU_PAINT_H
#define GPU_PAINT_H
#include <stdint.h>
#include <stdbool.h>

enum { PAINT_RGB, PAINT_RGBA, PAINT_RGBA_PREMULTIPLIED, PAINT_ALPHA };

bool paint_begin(uint32_t *layer, uint32_t width, uint32_t height, uint32_t stride_px);
void paint_rect(float x0, float y0, float x1, float y1, const int32_t clip[4], uint32_t color, bool copy);
bool paint_texture(const uint8_t *data, uint32_t stride, uint32_t width, uint32_t height, int format, int dx, int dy,
		   int dw, int dh, const int32_t clip[4], uint32_t tint, bool colorize);
bool paint_rounded(float x0, float y0, float x1, float y1, const float radii[4], float border, const int32_t clip[4],
		   uint32_t fill, uint32_t edge);
void paint_cpu(uint32_t y0, uint32_t y1);
void paint_sync(void);
void paint_end(void);
void paint_finish(void);
#endif
