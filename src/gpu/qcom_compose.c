/* The drawing calls of draw.h, a3xx.h and paint.h on the Qualcomm
 * watches: the Adreno (a3xx.c, draw.c, paint.c) while it comes up and
 * passes its selftest, otherwise the CPU (soft.c). The choice is made
 * at start and for good: a GPU that fails to come back after sleep is
 * not tried again, and memory handed out for one is usable by the other,
 * as both are plain memory to the CPU. */
#include "asteroid.h"
#include "gpu/a3xx.h"
#include "gpu/adreno.h"
#include "gpu/soft.h"

static bool cpu;

static void use_cpu(void)
{
	adreno_suspend();
	cpu = true;
	log_printf("gpu: composing on the CPU\n");
}

bool gpu_init(void)
{
	if (!cpu && !adreno_init())
		use_cpu();
	return cpu ? soft_init() : true;
}

bool gpu_selftest(void)
{
	if (!cpu && !adreno_selftest())
		use_cpu();
	return cpu ? soft_selftest() : true;
}

void gpu_suspend(void)
{
	if (cpu)
		soft_suspend();
	else
		adreno_suspend();
}

bool gpu_ready(void) { return !cpu && adreno_ready(); }

bool gpu_draw_untouched(void) { return false; }

void *gpu_alloc(uint32_t size, uint32_t align)
{
	return cpu ? soft_alloc(size, align) : adreno_alloc(size, align);
}

void gpu_draw_begin(const struct gpu_target *t)
{
	if (cpu)
		soft_draw_begin(t);
	else
		adreno_draw_begin(t);
}

bool gpu_draw_program(struct gpu_shader *vs, struct gpu_shader *fs)
{
	return cpu ? soft_draw_program(vs, fs) : adreno_draw_program(vs, fs);
}

void gpu_draw_consts(int stage, unsigned first_vec4, const float *v, unsigned nvec4)
{
	if (!cpu)
		adreno_draw_consts(stage, first_vec4, v, nvec4);
}

void gpu_draw_texture(int unit, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch_bytes)
{
	if (!cpu)
		adreno_draw_texture(unit, addr, width, height, pitch_bytes);
}

void gpu_draw_texture_format(int unit, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch_bytes,
			     enum gpu_tex format)
{
	if (!cpu)
		adreno_draw_texture_format(unit, addr, width, height, pitch_bytes, format);
}

void gpu_draw_invalidate_textures(void)
{
	if (cpu)
		soft_draw_invalidate_textures();
	else
		adreno_draw_invalidate_textures();
}

void gpu_draw_blend(bool premultiplied_over)
{
	if (cpu)
		soft_draw_blend(premultiplied_over);
	else
		adreno_draw_blend(premultiplied_over);
}

void gpu_draw_triangles(uint32_t vbo_addr, unsigned stride, unsigned nattrs, const uint8_t *attr_ncomp, unsigned nverts)
{
	if (cpu)
		soft_draw_triangles(vbo_addr, stride, nattrs, attr_ncomp, nverts);
	else
		adreno_draw_triangles(vbo_addr, stride, nattrs, attr_ncomp, nverts);
}

void gpu_draw_layer(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes)
{
	if (cpu)
		soft_draw_layer(tex_addr, width, height, pitch_bytes);
	else
		adreno_draw_layer(tex_addr, width, height, pitch_bytes);
}

void gpu_draw_layer_at(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha)
{
	if (cpu)
		soft_draw_layer_at(tex_addr, width, height, pitch_bytes, x, y, alpha);
	else
		adreno_draw_layer_at(tex_addr, width, height, pitch_bytes, x, y, alpha);
}

void gpu_draw_layer_parts(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha,
			  const struct gpu_rect *parts, unsigned n)
{
	if (cpu)
		soft_draw_layer_parts(tex_addr, width, height, pitch_bytes, x, y, alpha, parts, n);
	else
		adreno_draw_layer_parts(tex_addr, width, height, pitch_bytes, x, y, alpha, parts, n);
}

void gpu_draw_layer_round(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha)
{
	if (cpu)
		soft_draw_layer_round(tex_addr, width, height, pitch_bytes, x, y, alpha);
	else
		adreno_draw_layer_round(tex_addr, width, height, pitch_bytes, x, y, alpha);
}

void gpu_draw_turn(float c, float s)
{
	if (cpu)
		soft_draw_turn(c, s);
	else
		adreno_draw_turn(c, s);
}

void gpu_draw_turn_vertices(float *v, unsigned n, unsigned stride)
{
	if (cpu)
		soft_draw_turn_vertices(v, n, stride);
	else
		adreno_draw_turn_vertices(v, n, stride);
}

void gpu_draw_cpu_sync(void *buf, uint32_t bytes)
{
	if (cpu)
		soft_draw_cpu_sync(buf, bytes);
	else
		adreno_draw_cpu_sync(buf, bytes);
}

void gpu_draw_submit(void)
{
	if (cpu)
		soft_draw_submit();
	else
		adreno_draw_submit();
}

bool gpu_draw_wait(void)
{
	return cpu ? soft_draw_wait() : adreno_draw_wait();
}

bool gpu_draw_end(void)
{
	return cpu ? soft_draw_end() : adreno_draw_end();
}

void paint_rounded_clip(const float *clip)
{
	if (cpu)
		soft_paint_rounded_clip(clip);
	else
		adreno_paint_rounded_clip(clip);
}

bool paint_begin(uint32_t *layer, uint32_t width, uint32_t height, uint32_t stride_px)
{
	return cpu ? soft_paint_begin(layer, width, height, stride_px) : adreno_paint_begin(layer, width, height, stride_px);
}

void paint_rect(float x0, float y0, float x1, float y1, const int32_t clip[4], uint32_t color, bool copy)
{
	if (cpu)
		soft_paint_rect(x0, y0, x1, y1, clip, color, copy);
	else
		adreno_paint_rect(x0, y0, x1, y1, clip, color, copy);
}

bool paint_texture(const uint8_t *data, uint32_t stride, uint32_t width, uint32_t height, int format, int dx, int dy,
		   int dw, int dh, const int32_t clip[4], uint32_t tint, bool colorize)
{
	return cpu ? soft_paint_texture(data, stride, width, height, format, dx, dy, dw, dh, clip, tint, colorize)
		   : adreno_paint_texture(data, stride, width, height, format, dx, dy, dw, dh, clip, tint, colorize);
}

bool paint_rounded(float x0, float y0, float x1, float y1, const float radii[4], float border, const int32_t clip[4],
		   uint32_t fill, uint32_t edge)
{
	return cpu ? soft_paint_rounded(x0, y0, x1, y1, radii, border, clip, fill, edge)
		   : adreno_paint_rounded(x0, y0, x1, y1, radii, border, clip, fill, edge);
}

void paint_cpu(uint32_t y0, uint32_t y1)
{
	if (cpu)
		soft_paint_cpu(y0, y1);
	else
		adreno_paint_cpu(y0, y1);
}

void paint_sync(void)
{
	if (cpu)
		soft_paint_sync();
	else
		adreno_paint_sync();
}

void paint_end(void)
{
	if (cpu)
		soft_paint_end();
	else
		adreno_paint_end();
}

void paint_finish(void)
{
	if (cpu)
		soft_paint_finish();
	else
		adreno_paint_finish();
}
