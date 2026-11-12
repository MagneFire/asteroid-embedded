/* The drawing calls of draw.h and a3xx.h on the CPU (soft.c), under
 * names of their own: on minnow they are what the GPU's calls fall back
 * to (sgx_compose.c). */
#ifndef GPU_SOFT_H
#define GPU_SOFT_H
#include "gpu/draw.h"

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
#endif
