/* The Adreno 305B's side of the drawing calls of draw.h and a3xx.h, and
 * of the UI's paint.h, under names of their own: on the Qualcomm watches
 * qcom_compose.c forwards the calls to these while the GPU works and to
 * the CPU's (soft.h) when it does not. The calls are documented in those
 * headers. */
#ifndef GPU_ADRENO_H
#define GPU_ADRENO_H
#include "gpu/draw.h"
#include "gpu/paint.h"

bool adreno_init(void);
bool adreno_selftest(void);
void adreno_suspend(void);
bool adreno_ready(void);
void *adreno_alloc(uint32_t size, uint32_t align);
void adreno_draw_begin(const struct gpu_target *t);
bool adreno_draw_program(struct gpu_shader *vs, struct gpu_shader *fs);
void adreno_draw_consts(int stage, unsigned first_vec4, const float *v, unsigned nvec4);
void adreno_draw_texture(int unit, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch_bytes);
void adreno_draw_texture_format(int unit, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch_bytes,
				enum gpu_tex format);
void adreno_draw_invalidate_textures(void);
void adreno_draw_blend(bool premultiplied_over);
void adreno_draw_triangles(uint32_t vbo_addr, unsigned stride, unsigned nattrs, const uint8_t *attr_ncomp, unsigned nverts);
void adreno_draw_layer(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes);
void adreno_draw_layer_at(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha);
void adreno_draw_layer_parts(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y,
			     float alpha, const struct gpu_rect *parts, unsigned n);
void adreno_draw_layer_round(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y,
			     float alpha);
void adreno_draw_turn(float c, float s);
void adreno_draw_turn_vertices(float *v, unsigned n, unsigned stride);
void adreno_draw_cpu_sync(void *buf, uint32_t bytes);
void adreno_draw_submit(void);
bool adreno_draw_wait(void);
bool adreno_draw_end(void);

void adreno_paint_rounded_clip(const float *clip);
bool adreno_paint_begin(uint32_t *layer, uint32_t width, uint32_t height, uint32_t stride_px);
void adreno_paint_rect(float x0, float y0, float x1, float y1, const int32_t clip[4], uint32_t color, bool copy);
bool adreno_paint_texture(const uint8_t *data, uint32_t stride, uint32_t width, uint32_t height, int format, int dx, int dy,
			  int dw, int dh, const int32_t clip[4], uint32_t tint, bool colorize);
bool adreno_paint_rounded(float x0, float y0, float x1, float y1, const float radii[4], float border, const int32_t clip[4],
			  uint32_t fill, uint32_t edge);
void adreno_paint_cpu(uint32_t y0, uint32_t y1);
void adreno_paint_sync(void);
void adreno_paint_end(void);
void adreno_paint_finish(void);
#endif
