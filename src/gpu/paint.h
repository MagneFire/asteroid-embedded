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
 * so far, after which the CPU may read them. paint_rounded_clip limits
 * what follows to a rounded rectangle, clip being x0, y0, x1, y1 and the
 * radius, or lifts that with NULL. */
#ifndef GPU_PAINT_H
#define GPU_PAINT_H
#include <stdint.h>
#include <stdbool.h>

enum { PAINT_RGB, PAINT_RGBA, PAINT_RGBA_PREMULTIPLIED, PAINT_ALPHA };

void paint_rounded_clip(const float *clip);
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

/* On minnow only. paint_mode picks who paints (sgx_paint.c): the GPU,
 * the CPU (soft.c), or nobody, every call drawing nothing so that a
 * repaint costs only the UI's walk of its items; paint_gpu_lost says the
 * GPU failed a scene and the CPU paints until paint_mode(PAINT_MODE_GPU).
 * paint_refused counts an item the UI kept for itself before asking, of
 * a PAINT_REFUSED_* kind, and paint_log logs the last second's counts. */
enum { PAINT_MODE_SOFT, PAINT_MODE_GPU, PAINT_MODE_NULL };
enum { PAINT_REFUSED_BRUSH, PAINT_REFUSED_ROTATED, PAINT_REFUSED_TILED, PAINT_REFUSED_ALPHA_MAP, PAINT_REFUSED_FORMAT,
       PAINT_REFUSED_KINDS };

void paint_mode(int mode);
int paint_mode_now(void);
bool paint_gpu_lost(void);
void paint_null(bool on);
bool paint_null_on(void);
void paint_refused(uint32_t kind);
void paint_log(void);
/* With overlap on (the default), a repaint's last scene is not waited
 * for by paint_finish but by paint_wait, or by the next CPU write to
 * its layer. */
void paint_overlap(bool on);
bool paint_overlap_on(void);
void paint_wait(void);
/* New atlases of w x h from the next repaint on the GPU, false if the
 * GPU does not take that size; now_w x now_h is the size in use (0 x 0
 * when none is allocated). */
bool paint_atlas(uint32_t w, uint32_t h);
void paint_atlas_size(uint32_t *w, uint32_t *h, uint32_t *now_w, uint32_t *now_h);
/* What the layer's repaints drew since the last call, per band of
 * PAINT_BAND rows: the columns x0..x1 that may hold content (x0 >= x1
 * for none), the columns c0..c1 whose pixels a copy replaced in all of
 * the band's rows, and in scan the bands whose rows the UI wrote
 * itself; false if not known. */
#define PAINT_BAND 16
#define PAINT_BANDS 32
struct paint_bounds {
	int16_t x0[PAINT_BANDS], x1[PAINT_BANDS], c0[PAINT_BANDS], c1[PAINT_BANDS];
	uint32_t scan;
};
bool paint_bounds(const uint32_t *layer, struct paint_bounds *b);
#endif
