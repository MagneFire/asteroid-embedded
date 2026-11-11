/* The drawing calls of draw.h on the CPU, for a watch whose GPU the
 * firmware does not drive: the frame is put together from the mesh's
 * triangles and the UI's layers with NEON. Only what the frame needs is
 * here. The triangles are the mesh's, one colour each, taken from the
 * first vertex. A pixel belongs to a triangle if its centre is inside,
 * with the left and top edges counted in, so triangles that share an
 * edge neither leave a gap nor draw a pixel twice, which blending would
 * show. The picture is turned for the nightstand once it is whole, by
 * quarter turns. The UI draws its layers itself: paint_begin says no. */
#include <string.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif
#include "asteroid.h"
#include "gpu/a3xx.h"
#include "gpu/draw.h"
#include "gpu/paint.h"
#include "ui/rotation.h"

#define POOL_SIZE	(192u * 1024)

/* Addresses are 32 bits in the drawing calls, which on a host with wider
 * pointers a test counts from a base of its own. */
#ifdef HOST_TEST
extern uintptr_t gpu_host_base;
#define PTR(addr)	((void *)(gpu_host_base + (addr)))
#else
#define PTR(addr)	((void *)(uintptr_t)(addr))
#endif

static uint8_t pool[POOL_SIZE] __attribute__((aligned(64)));
static uint32_t pool_used;
static struct gpu_target target;
static bool blend;

bool gpu_init(void) { return true; }
bool gpu_selftest(void) { return true; }
void gpu_suspend(void) {}
bool gpu_ready(void) { return false; }

void *gpu_alloc(uint32_t size, uint32_t align)
{
	uint32_t at = (pool_used + align - 1) & ~(align - 1);
	if (at + size > POOL_SIZE)
		return NULL;
	pool_used = at + size;
	return pool + at;
}

void gpu_draw_begin(const struct gpu_target *t)
{
	target = *t;
	blend = false;
}

bool gpu_draw_program(struct gpu_shader *vs, struct gpu_shader *fs) { return true; }
void gpu_draw_blend(bool premultiplied_over) { blend = premultiplied_over; }
void gpu_draw_turn(float c, float s) {}
void gpu_draw_turn_vertices(float *v, unsigned n, unsigned stride) {}
void gpu_draw_submit(void) {}
bool gpu_draw_wait(void) { return true; }
void gpu_draw_invalidate_textures(void) {}
void gpu_draw_cpu_sync(void *buf, uint32_t bytes) {}

bool gpu_draw_end(void)
{
	if (target.width == target.height)
		rotation_buffer(PTR(target.base), target.pitch_bytes / 4);
	return true;
}

static inline uint32_t *row(int y)
{
	return (uint32_t *)((uint8_t *)PTR(target.base) + (uint32_t)y * target.pitch_bytes);
}

/* v * by / 255, rounded. */
static inline uint32_t scale8(uint32_t v, uint32_t by)
{
	uint32_t t = v * by + 128;
	return (t + (t >> 8)) >> 8;
}

#ifdef __ARM_NEON
static inline uint8x8_t div255(uint16x8_t x)
{
	return vraddhn_u16(x, vrshrq_n_u16(x, 8));
}
#endif

/* color is premultiplied 0xAARRGGBB. */
static void fill(uint32_t *d, int n, uint32_t color)
{
	int i = 0;
	if (!blend || color >> 24 == 0xff) {
		color &= 0xffffff;
#ifdef __ARM_NEON
		uint32x4_t c = vdupq_n_u32(color);
		for (; i + 4 <= n; i += 4)
			vst1q_u32(d + i, c);
#endif
		for (; i < n; i++)
			d[i] = color;
		return;
	}
	if (!color)
		return;
	uint32_t keep = 255 - (color >> 24);
#ifdef __ARM_NEON
	uint8x8_t k = vdup_n_u8((uint8_t)keep);
	uint8x16_t add = vreinterpretq_u8_u32(vdupq_n_u32(color & 0xffffff));
	for (; i + 4 <= n; i += 4) {
		uint8x16_t p = vreinterpretq_u8_u32(vld1q_u32(d + i));
		uint8x16_t kept = vcombine_u8(div255(vmull_u8(vget_low_u8(p), k)), div255(vmull_u8(vget_high_u8(p), k)));
		vst1q_u32(d + i, vreinterpretq_u32_u8(vaddq_u8(kept, add)));
	}
#endif
	for (; i < n; i++) {
		uint32_t p = d[i];
		d[i] = (color & 0xffffff) + (scale8(p >> 16 & 0xff, keep) << 16 | scale8(p >> 8 & 0xff, keep) << 8 |
						scale8(p & 0xff, keep));
	}
}

/* Positions are in 65536ths of a pixel: the processor's own arithmetic
 * on fractions is far slower than on whole numbers. */
#define ONE	65536
#define HALF	32768

/* The first pixel, or row, whose centre is at or past v. */
static inline int first_px(int64_t v)
{
	return (int)((v + HALF - 1) >> 16);
}

/* An edge as its rows see it: the rows whose centres lie between its
 * ends, where it crosses the first of them and how far it moves on a
 * row. The two ends are put in one order first, so both triangles on an
 * edge get the same answers. */
struct edge {
	int y0, y1;
	int64_t x, step;
};

static void edge_set(struct edge *e, const int32_t *a, const int32_t *b)
{
	if (a[1] > b[1] || (a[1] == b[1] && a[0] > b[0])) {
		const int32_t *t = a;
		a = b;
		b = t;
	}
	e->y0 = first_px(a[1]);
	e->y1 = first_px(b[1]);
	if (e->y0 >= e->y1)
		return;
	int64_t dx = (int64_t)b[0] - a[0], dy = (int64_t)b[1] - a[1];
	e->step = dx * ONE / dy;
	e->x = a[0] + dx * ((int64_t)e->y0 * ONE + HALF - a[1]) / dy;
}

static inline int64_t edge_at(const struct edge *e, int y)
{
	return e->x + e->step * (y - e->y0);
}

static void triangle(int32_t p[3][2], uint32_t color)
{
	const int32_t *v0 = p[0], *v1 = p[1], *v2 = p[2], *t;
	if (v0[1] > v1[1]) { t = v0; v0 = v1; v1 = t; }
	if (v1[1] > v2[1]) { t = v1; v1 = v2; v2 = t; }
	if (v0[1] > v1[1]) { t = v0; v0 = v1; v1 = t; }
	struct edge whole, upper, lower;
	edge_set(&whole, v0, v2);
	edge_set(&upper, v0, v1);
	edge_set(&lower, v1, v2);
	int y0 = whole.y0 < 0 ? 0 : whole.y0, y1 = whole.y1 > (int)target.height ? (int)target.height : whole.y1;
	if (y0 >= y1)
		return;
	int64_t a = edge_at(&whole, y0);
	for (int y = y0; y < y1; y++, a += whole.step) {
		const struct edge *e = y < upper.y1 ? &upper : &lower;
		int64_t b = edge_at(e, y), lo = a < b ? a : b, hi = a < b ? b : a;
		if (lo < 0)
			lo = 0;
		if (hi > (int64_t)target.width * ONE)
			hi = (int64_t)target.width * ONE;
		int x0 = first_px(lo), x1 = first_px(hi);
		if (x0 < x1)
			fill(row(y) + x0, x1 - x0, color);
	}
}

/* A position far outside the frame is as good at its edge of what
 * fits, and is drawn the same from there. */
static int32_t to_fixed(float v)
{
	const float limit = 16000.0f * ONE;
	return (int32_t)(v < -limit ? -limit : v > limit ? limit : v);
}

static uint32_t channel(float v)
{
	int i = (int)(v * 255.0f + 0.5f);
	return i < 0 ? 0 : i > 255 ? 255 : (uint32_t)i;
}

/* Vertices are position (x, y, z, w) in clip space, row 0 at the top,
 * then colour (r, g, b, a). */
void gpu_draw_triangles(uint32_t vbo_addr, unsigned stride, unsigned nattrs, const uint8_t *attr_ncomp, unsigned nverts)
{
	if (nattrs != 2 || stride < 32)
		return;
	const uint8_t *v = PTR(vbo_addr);
	float w = (float)target.width * 0.5f * ONE, h = (float)target.height * 0.5f * ONE;
	for (unsigned i = 0; i + 3 <= nverts; i += 3) {
		int32_t p[3][2];
		for (int k = 0; k < 3; k++) {
			const float *f = (const float *)(const void *)(v + (i + (unsigned)k) * stride);
			p[k][0] = to_fixed((f[0] + 1.0f) * w);
			p[k][1] = to_fixed((1.0f - f[1]) * h);
		}
		const float *c = (const float *)(const void *)(v + i * stride) + 4;
		triangle(p, channel(c[3]) << 24 | channel(c[0]) << 16 | channel(c[1]) << 8 | channel(c[2]));
	}
}

/* n pixels of a premultiplied layer (bytes R, G, B, A) over the frame
 * (B, G, R, x), faded by alpha out of 256. */
static void over(uint32_t *d, const uint32_t *s, int n, uint32_t alpha)
{
	bool faded = alpha < 256;
	int i = 0;
#ifdef __ARM_NEON
	uint8x8_t fade = vdup_n_u8((uint8_t)(alpha > 255 ? 255 : alpha));
	for (; i + 8 <= n; i += 8) {
		uint8x8x4_t src = vld4_u8((const uint8_t *)(s + i));
		if (!vget_lane_u64(vreinterpret_u64_u8(src.val[3]), 0))
			continue;
		if (faded)
			for (int c = 0; c < 4; c++)
				src.val[c] = div255(vmull_u8(src.val[c], fade));
		uint8x8x4_t dst = vld4_u8((const uint8_t *)(d + i));
		uint8x8_t keep = vmvn_u8(src.val[3]);
		dst.val[0] = vqadd_u8(div255(vmull_u8(dst.val[0], keep)), src.val[2]);
		dst.val[1] = vqadd_u8(div255(vmull_u8(dst.val[1], keep)), src.val[1]);
		dst.val[2] = vqadd_u8(div255(vmull_u8(dst.val[2], keep)), src.val[0]);
		vst4_u8((uint8_t *)(d + i), dst);
	}
#endif
	uint32_t f = alpha > 255 ? 255 : alpha;
	for (; i < n; i++) {
		uint32_t p = s[i], q = d[i];
		uint32_t r = p & 0xff, g = p >> 8 & 0xff, b = p >> 16 & 0xff, a = p >> 24;
		if (!a)
			continue;
		if (faded) {
			r = scale8(r, f);
			g = scale8(g, f);
			b = scale8(b, f);
			a = scale8(a, f);
		}
		uint32_t keep = 255 - a;
		r += scale8(q >> 16 & 0xff, keep);
		g += scale8(q >> 8 & 0xff, keep);
		b += scale8(q & 0xff, keep);
		d[i] = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
	}
}

static uint32_t fade_of(float alpha)
{
	return alpha >= 1.0f ? 256 : alpha <= 0.0f ? 0 : (uint32_t)(alpha * 255.0f + 0.5f);
}

/* Columns x0 to x1 of one row of the layer, drawn with the layer's
 * corner at (x, y) of the frame. */
static void layer_span(const uint32_t *tex, uint32_t pitch_bytes, int x, int y, int ly, int x0, int x1, uint32_t fade)
{
	int ty = y + ly;
	if (ty < 0 || ty >= (int)target.height)
		return;
	if (x0 + x < 0)
		x0 = -x;
	if (x1 + x > (int)target.width)
		x1 = (int)target.width - x;
	if (x0 < x1)
		over(row(ty) + x + x0, (const uint32_t *)(const void *)((const uint8_t *)tex + (uint32_t)ly * pitch_bytes) + x0,
		     x1 - x0, fade);
}

void gpu_draw_layer_parts(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha,
			  const struct gpu_rect *parts, unsigned n)
{
	const uint32_t *tex = PTR(tex_addr);
	uint32_t fade = fade_of(alpha);
	if (!fade)
		return;
	for (unsigned i = 0; i < n; i++) {
		int x0 = parts[i].x < 0 ? 0 : parts[i].x, x1 = parts[i].x + (int)parts[i].width;
		int y0 = parts[i].y < 0 ? 0 : parts[i].y, y1 = parts[i].y + (int)parts[i].height;
		if (x1 > (int)width)
			x1 = (int)width;
		if (y1 > (int)height)
			y1 = (int)height;
		for (int ly = y0; ly < y1; ly++)
			layer_span(tex, pitch_bytes, x, y, ly, x0, x1, fade);
	}
}

void gpu_draw_layer_at(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha)
{
	struct gpu_rect whole = { 0, 0, width, height };
	gpu_draw_layer_parts(tex_addr, width, height, pitch_bytes, x, y, alpha, &whole, 1);
}

void gpu_draw_layer(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes)
{
	gpu_draw_layer_at(tex_addr, width, height, pitch_bytes, 0, 0, 1.0f);
}

/* The square root of v, rounded down. */
static int isqrt(int v)
{
	int r = 0;
	for (int bit = 1 << 14; bit; bit >>= 1)
		if ((r + bit) * (r + bit) <= v)
			r += bit;
	return r;
}

/* What is inside the circle the layer's width across, around the
 * layer's centre. */
void gpu_draw_layer_round(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha)
{
	const uint32_t *tex = PTR(tex_addr);
	uint32_t fade = fade_of(alpha);
	if (!fade)
		return;
	/* In half pixels, so a row's centre is a whole number. */
	int d = (int)width, cy = (int)height;
	for (int ly = 0; ly < (int)height; ly++) {
		int dy = 2 * ly + 1 - cy;
		if (dy * dy >= d * d)
			continue;
		int half = isqrt(d * d - dy * dy);
		layer_span(tex, pitch_bytes, x, y, ly, (d - half) / 2, (d + half + 1) / 2, fade);
	}
}

bool paint_begin(uint32_t *layer, uint32_t width, uint32_t height, uint32_t stride_px) { return false; }
void paint_rounded_clip(const float *clip) {}
void paint_rect(float x0, float y0, float x1, float y1, const int32_t clip[4], uint32_t color, bool copy) {}
bool paint_texture(const uint8_t *data, uint32_t stride, uint32_t width, uint32_t height, int format, int dx, int dy,
		   int dw, int dh, const int32_t clip[4], uint32_t tint, bool colorize)
{
	return false;
}
bool paint_rounded(float x0, float y0, float x1, float y1, const float radii[4], float border, const int32_t clip[4],
		   uint32_t fill_color, uint32_t edge)
{
	return false;
}
void paint_cpu(uint32_t y0, uint32_t y1) {}
void paint_sync(void) {}
void paint_end(void) {}
void paint_finish(void) {}
