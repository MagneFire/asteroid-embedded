/* The drawing calls of draw.h and a3xx.h and the UI's of paint.h on the
 * CPU (soft.c), under names of their own: they are what the GPU's calls
 * fall back to (sgx_compose.c, qcom_compose.c). */
#ifndef GPU_SOFT_H
#define GPU_SOFT_H
#include "gpu/draw.h"
#include "gpu/paint.h"

bool soft_init(void);
bool soft_selftest(void);
void soft_suspend(void);
bool soft_ready(void);
void *soft_alloc(uint32_t size, uint32_t align);
/* Where soft_alloc takes its memory from, page aligned. */
void *soft_pool(uint32_t *size);
void soft_draw_begin(const struct gpu_target *t);
bool soft_draw_program(struct gpu_shader *vs, struct gpu_shader *fs);
void soft_draw_blend(bool premultiplied_over);
void soft_draw_turn(float c, float s);
void soft_draw_turn_vertices(float *v, unsigned n, unsigned stride);
void soft_draw_submit(void);
bool soft_draw_wait(void);
void soft_draw_invalidate_textures(void);
void soft_draw_cpu_sync(void *buf, uint32_t bytes);
bool soft_draw_end(void);
void soft_draw_triangles(uint32_t vbo_addr, unsigned stride, unsigned nattrs, const uint8_t *attr_ncomp, unsigned nverts);
void soft_draw_layer_parts(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha,
			   const struct gpu_rect *parts, unsigned n);
void soft_draw_layer_at(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha);
void soft_draw_layer(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes);
void soft_draw_layer_round(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha);

void soft_paint_rounded_clip(const float *clip);
bool soft_paint_begin(uint32_t *layer, uint32_t width, uint32_t height, uint32_t stride_px);
void soft_paint_rect(float x0, float y0, float x1, float y1, const int32_t clip[4], uint32_t color, bool copy);
bool soft_paint_texture(const uint8_t *data, uint32_t stride, uint32_t width, uint32_t height, int format, int dx, int dy,
			int dw, int dh, const int32_t clip[4], uint32_t tint, bool colorize);
bool soft_paint_rounded(float x0, float y0, float x1, float y1, const float radii[4], float border, const int32_t clip[4],
			uint32_t fill, uint32_t edge);
void soft_paint_cpu(uint32_t y0, uint32_t y1);
void soft_paint_sync(void);
void soft_paint_end(void);
void soft_paint_finish(void);
#endif
