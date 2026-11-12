/* The drawing calls of draw.h on the CPU (soft.h), for when the GPU does
 * not draw the frame: the frame is put together from the mesh's
 * triangles and the UI's layers with NEON. Only what the frame needs is
 * here. The triangles are the mesh's, one colour each, taken from the
 * first vertex. A pixel belongs to a triangle if its centre is inside,
 * with the left and top edges counted in, so triangles that share an
 * edge neither leave a gap nor draw a pixel twice, which blending would
 * show. The picture is turned for the nightstand once it is whole, by
 * quarter turns. */
#include <string.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif
#include "asteroid.h"
#include "gpu/soft.h"
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

/* Page aligned, so that the GPU can map it. */
static uint8_t pool[POOL_SIZE] __attribute__((aligned(4096)));
static uint32_t pool_used;
static struct gpu_target target;
static bool blend;

bool soft_init(void) { return true; }
bool soft_selftest(void) { return true; }
void soft_suspend(void) {}
bool soft_ready(void) { return false; }

void *soft_alloc(uint32_t size, uint32_t align)
{
	uint32_t at = (pool_used + align - 1) & ~(align - 1);
	if (at + size > POOL_SIZE)
		return NULL;
	pool_used = at + size;
	return pool + at;
}

void *soft_pool(uint32_t *size)
{
	*size = POOL_SIZE;
	return pool;
}

void soft_draw_begin(const struct gpu_target *t)
{
	target = *t;
	blend = false;
}

bool soft_draw_program(struct gpu_shader *vs, struct gpu_shader *fs) { return true; }
void soft_draw_blend(bool premultiplied_over) { blend = premultiplied_over; }
/* How far the picture is turned, as the 65536ths of its cosine and
 * sine. */
static int32_t turn_c = 65536, turn_s;

void soft_draw_turn(float c, float s)
{
	turn_c = (int32_t)(c * 65536.0f + (c < 0.0f ? -0.5f : 0.5f));
	turn_s = (int32_t)(s * 65536.0f + (s < 0.0f ? -0.5f : 0.5f));
}
void soft_draw_turn_vertices(float *v, unsigned n, unsigned stride) {}
void soft_draw_submit(void) {}
bool soft_draw_wait(void) { return true; }
void soft_draw_invalidate_textures(void) {}
void soft_draw_cpu_sync(void *buf, uint32_t bytes) {}

#define TURN_MAX	320

/* The whole picture turned clockwise about its centre while it is on its
 * way from one quarter turn to another: each pixel takes the one that
 * lands on it, and what comes from outside the picture is black. */
static void turn_frame(void)
{
	static uint32_t upright[TURN_MAX * TURN_MAX];
	int size = (int)target.width;
	uint32_t pitch = target.pitch_bytes / 4, *frame = PTR(target.base);
	for (int y = 0; y < size; y++)
		memcpy(upright + y * size, frame + (uint32_t)y * pitch, (size_t)size * 4);
	/* Twice the distance from the centre, so that it is whole. */
	int32_t mid = size - 1;
	for (int y = 0; y < size; y++) {
		int32_t dy = 2 * y - mid, dx = -mid;
		/* The upright pixel under the first of the row, in 65536ths. */
		int32_t u = (mid * 32768) + (dx * (turn_c / 2)) + (dy * (turn_s / 2)) + 32768;
		int32_t v = (mid * 32768) - (dx * (turn_s / 2)) + (dy * (turn_c / 2)) + 32768;
		uint32_t *d = frame + (uint32_t)y * pitch;
		for (int x = 0; x < size; x++, u += turn_c, v -= turn_s) {
			int sx = u >> 16, sy = v >> 16;
			d[x] = sx >= 0 && sx < size && sy >= 0 && sy < size ? upright[sy * size + sx] : 0;
		}
	}
}

bool soft_draw_end(void)
{
	if (target.width != target.height)
		return true;
	bool quarter = (turn_c == 0 || turn_s == 0) && (turn_c == 65536 || turn_c == -65536 || turn_s == 65536 || turn_s == -65536);
	if (quarter || target.width > TURN_MAX)
		rotation_buffer(PTR(target.base), target.pitch_bytes / 4);
	else
		turn_frame();
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
void soft_draw_triangles(uint32_t vbo_addr, unsigned stride, unsigned nattrs, const uint8_t *attr_ncomp, unsigned nverts)
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

void soft_draw_layer_parts(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha,
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

void soft_draw_layer_at(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha)
{
	struct gpu_rect whole = { 0, 0, width, height };
	soft_draw_layer_parts(tex_addr, width, height, pitch_bytes, x, y, alpha, &whole, 1);
}

void soft_draw_layer(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes)
{
	soft_draw_layer_at(tex_addr, width, height, pitch_bytes, 0, 0, 1.0f);
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
void soft_draw_layer_round(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha)
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

/* The UI's own drawing into a layer, premultiplied pixels of bytes R, G,
 * B, A: rectangles, flat or with rounded corners, and textures, which
 * is the glyphs and the pictures. Rounded rectangles with a border and
 * glyphs at another size the UI is left to draw itself, a pixel at a
 * time. */
static struct {
	uint8_t *px;
	int width, height;
	uint32_t pitch;
	/* A rounded rectangle that what is drawn stays inside. */
	bool rounded;
	int rx0, ry0, rx1, ry1;
	float radius;
} paint;

bool soft_paint_begin(uint32_t *layer, uint32_t width, uint32_t height, uint32_t stride_px)
{
	paint.px = (uint8_t *)layer;
	paint.width = (int)width;
	paint.height = (int)height;
	paint.pitch = stride_px * 4;
	paint.rounded = false;
	return true;
}

static int round_px(float v)
{
	return (int)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

void soft_paint_rounded_clip(const float *clip)
{
	paint.rounded = clip != NULL;
	if (!clip)
		return;
	paint.rx0 = round_px(clip[0]);
	paint.ry0 = round_px(clip[1]);
	paint.rx1 = round_px(clip[2]);
	paint.ry1 = round_px(clip[3]);
	float w = (float)(paint.rx1 - paint.rx0), h = (float)(paint.ry1 - paint.ry0), most = (w < h ? w : h) * 0.5f;
	paint.radius = clip[4] < 0.0f ? 0.0f : clip[4] > most ? most : clip[4];
}

static bool paint_clip(int *x0, int *y0, int *x1, int *y1, const int32_t clip[4])
{
	if (*x0 < clip[0])
		*x0 = clip[0];
	if (*y0 < clip[1])
		*y0 = clip[1];
	if (*x1 > clip[2])
		*x1 = clip[2];
	if (*y1 > clip[3])
		*y1 = clip[3];
	if (*x0 < 0)
		*x0 = 0;
	if (*y0 < 0)
		*y0 = 0;
	if (*x1 > paint.width)
		*x1 = paint.width;
	if (*y1 > paint.height)
		*y1 = paint.height;
	if (paint.rounded) {
		if (*x0 < paint.rx0)
			*x0 = paint.rx0;
		if (*y0 < paint.ry0)
			*y0 = paint.ry0;
		if (*x1 > paint.rx1)
			*x1 = paint.rx1;
		if (*y1 > paint.ry1)
			*y1 = paint.ry1;
	}
	return *x0 < *x1 && *y0 < *y1;
}

/* The square root, by the processor's own instruction where the
 * compiler would call a routine for it. */
static inline float root(float v)
{
#if defined(__arm__) && !defined(HOST_TEST)
	float r;
	__asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(v));
	return r;
#else
	return __builtin_sqrtf(v);
#endif
}

/* How far in from the side of its square a corner's arc is, dy away
 * from the height of the arc's centre. */
static float arc_inset(float r, float dy)
{
	float inside = r * r - dy * dy;
	return inside <= 0.0f ? r : r - root(inside);
}

/* A corner's arc where it crosses a row: the pixels from the side that
 * are outside (below lo), cut by the arc (lo up to hi) and inside. */
struct arc {
	float r, dy;
	int lo, hi;
};

static void arc_set(struct arc *a, float r, float dy)
{
	a->r = r;
	a->dy = dy;
	a->lo = (int)arc_inset(r, dy > 0.5f ? dy - 0.5f : 0.0f);
	a->hi = (int)(arc_inset(r, dy + 0.5f) + 0.999f);
}

/* How much of the pixel i in from the side is inside, out of 255. */
static uint32_t arc_cover(const struct arc *a, int i)
{
	if (i >= a->hi)
		return 255;
	if (i < a->lo)
		return 0;
	float dx = a->r - ((float)i + 0.5f), cover = a->r + 0.5f - root(dx * dx + a->dy * a->dy);
	return cover <= 0.0f ? 0 : cover >= 1.0f ? 255 : (uint32_t)(cover * 255.0f + 0.5f);
}

/* The rounded clip in the row y: whether a corner's arc crosses it, and
 * from where to where pixels are wholly inside. */
static bool clip_arc(int y, struct arc *a, int *whole0, int *whole1)
{
	*whole0 = 0;
	*whole1 = paint.width;
	if (!paint.rounded)
		return false;
	*whole0 = paint.rx0;
	*whole1 = paint.rx1;
	float yc = (float)(y - paint.ry0) + 0.5f, h = (float)(paint.ry1 - paint.ry0), r = paint.radius;
	if (r <= 0.0f || (yc >= r && yc <= h - r))
		return false;
	arc_set(a, r, yc < r ? r - yc : yc - (h - r));
	*whole0 += a->hi;
	*whole1 -= a->hi;
	return true;
}

/* How much of the pixel x of that row the rounded clip leaves. */
static uint32_t clip_cover(const struct arc *a, int x)
{
	int left = x - paint.rx0, right = paint.rx1 - 1 - x;
	return arc_cover(a, left < right ? left : right);
}

/* One premultiplied pixel over another. */
static inline void px_over(uint8_t *d, const uint8_t s[4])
{
	uint32_t keep = 255u - s[3];
	for (int c = 0; c < 4; c++) {
		uint32_t v = s[c] + scale8(d[c], keep);
		d[c] = (uint8_t)(v > 255 ? 255 : v);
	}
}

/* The same for a pixel only part of which is drawn on. */
static void px_cover(uint8_t *d, const uint8_t s[4], uint32_t cover)
{
	if (!cover)
		return;
	if (cover >= 255) {
		px_over(d, s);
		return;
	}
	uint8_t part[4] = { (uint8_t)scale8(s[0], cover), (uint8_t)scale8(s[1], cover), (uint8_t)scale8(s[2], cover),
			    (uint8_t)scale8(s[3], cover) };
	px_over(d, part);
}

#ifdef __ARM_NEON
static inline void px8_over(uint8_t *d, uint8x8x4_t s)
{
	uint8x8x4_t dst = vld4_u8(d);
	uint8x8_t keep = vmvn_u8(s.val[3]);
	for (int c = 0; c < 4; c++)
		dst.val[c] = vqadd_u8(div255(vmull_u8(dst.val[c], keep)), s.val[c]);
	vst4_u8(d, dst);
}
#endif

/* n pixels of one colour, put there or blended over what is there. */
static void rect_row(uint8_t *d, int n, uint32_t color, bool put)
{
	uint8_t s[4] = { (uint8_t)color, (uint8_t)(color >> 8), (uint8_t)(color >> 16), (uint8_t)(color >> 24) };
	int i = 0;
#ifdef __ARM_NEON
	if (put) {
		uint32x4_t c = vdupq_n_u32(color);
		for (; i + 4 <= n; i += 4)
			vst1q_u8(d + i * 4, vreinterpretq_u8_u32(c));
	} else {
		uint8x8x4_t src = { { vdup_n_u8(s[0]), vdup_n_u8(s[1]), vdup_n_u8(s[2]), vdup_n_u8(s[3]) } };
		for (; i + 8 <= n; i += 8)
			px8_over(d + i * 4, src);
	}
#endif
	for (; i < n; i++) {
		if (put)
			memcpy(d + i * 4, s, 4);
		else
			px_over(d + i * 4, s);
	}
}

/* One colour for the pixels x0 to x1 of the row y, as far as the
 * rounded clip leaves them. With copy the colour replaces what is
 * there; where the clip cuts a pixel that is by the part it leaves. */
static void rect_span(int y, int x0, int x1, uint32_t color, bool copy)
{
	uint8_t *row = paint.px + (uint32_t)y * paint.pitch;
	uint8_t s[4] = { (uint8_t)color, (uint8_t)(color >> 8), (uint8_t)(color >> 16), (uint8_t)(color >> 24) };
	struct arc a;
	int w0, w1;
	bool cut = clip_arc(y, &a, &w0, &w1);
	if (w0 < x0)
		w0 = x0;
	if (w1 > x1)
		w1 = x1;
	if (w0 < w1)
		rect_row(row + (uint32_t)w0 * 4, w1 - w0, color, copy || s[3] == 255);
	else
		w0 = w1 = x1;
	if (!cut)
		return;
	for (int x = x0 == w0 ? w1 : x0; x < x1; x = x + 1 == w0 ? w1 : x + 1) {
		uint32_t cover = clip_cover(&a, x);
		uint8_t *d = row + (uint32_t)x * 4;
		if (copy && cover)
			for (int c = 0; c < 4; c++)
				d[c] = (uint8_t)(scale8(s[c], cover) + scale8(d[c], 255 - cover));
		else
			px_cover(d, s, cover);
	}
}

void soft_paint_rect(float fx0, float fy0, float fx1, float fy1, const int32_t clip[4], uint32_t color, bool copy)
{
	int x0 = round_px(fx0), y0 = round_px(fy0), x1 = round_px(fx1), y1 = round_px(fy1);
	if (!paint_clip(&x0, &y0, &x1, &y1, clip) || (!copy && !color))
		return;
	for (int y = y0; y < y1; y++)
		rect_span(y, x0, x1, color, copy);
}

/* How a texel becomes a premultiplied pixel: its alpha alone times the
 * tint, or its colour, premultiplied if it is not yet, times the tint. */
enum { TEXEL_ALPHA, TEXEL_PREMULTIPLIED, TEXEL_STRAIGHT, TEXEL_RGB };

struct texels {
	unsigned bytes;
	int kind;
	uint8_t tint[4];
	bool tinted;
};

/* One texel as a pixel, false if there is nothing of it. */
static inline bool texel_px(const struct texels *k, const uint8_t *q, uint8_t p[4])
{
	if (k->kind == TEXEL_ALPHA) {
		uint32_t a = k->bytes == 1 ? q[0] : k->bytes == 3 ? 255 : q[3];
		if (!a)
			return false;
		for (int c = 0; c < 4; c++)
			p[c] = (uint8_t)scale8(a, k->tint[c]);
		return true;
	}
	p[3] = k->kind == TEXEL_RGB ? 255 : q[3];
	for (int c = 0; c < 3; c++)
		p[c] = k->kind == TEXEL_STRAIGHT ? (uint8_t)scale8(q[c], p[3]) : q[c];
	if (k->tinted)
		for (int c = 0; c < 4; c++)
			p[c] = (uint8_t)scale8(p[c], k->tint[c]);
	return p[3] != 0;
}

/* n texels in a row over the layer. The loop is picked once for the row;
 * a group of eight that is all transparent is skipped, and one that is
 * all opaque and untinted is stored as it is. */
static void texture_row(uint8_t *d, const uint8_t *src, int n, const struct texels *k)
{
	unsigned bytes = k->bytes;
	const uint8_t *t = k->tint;
	int i = 0;
	/* Without alpha of their own the texels of a colour are all opaque,
	 * and so the tint is the pixel. */
	if (k->kind == TEXEL_ALPHA && bytes == 3) {
		rect_row(d, n, t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24, t[3] == 255);
		return;
	}
#ifdef __ARM_NEON
	uint8x8_t t0 = vdup_n_u8(t[0]), t1 = vdup_n_u8(t[1]), t2 = vdup_n_u8(t[2]), t3 = vdup_n_u8(t[3]);
	if (k->kind == TEXEL_ALPHA) {
		uint8x8x4_t solid = { { t0, t1, t2, t3 } };
		for (; i + 8 <= n; i += 8) {
			const uint8_t *q = src + (unsigned)i * bytes;
			uint8x8_t a;
			if (bytes == 1) {
				a = vld1_u8(q);
			} else {
				/* The top byte of eight words, without taking them apart. */
				uint32x4_t w0 = vreinterpretq_u32_u8(vld1q_u8(q)), w1 = vreinterpretq_u32_u8(vld1q_u8(q + 16));
				a = vshrn_n_u16(vcombine_u16(vshrn_n_u32(w0, 16), vshrn_n_u32(w1, 16)), 8);
			}
			uint64_t all = vget_lane_u64(vreinterpret_u64_u8(a), 0);
			if (!all)
				continue;
			if (all == ~0ull && t[3] == 255) {
				vst4_u8(d + i * 4, solid);
				continue;
			}
			uint8x8x4_t p;
			p.val[0] = div255(vmull_u8(a, t0));
			p.val[1] = div255(vmull_u8(a, t1));
			p.val[2] = div255(vmull_u8(a, t2));
			p.val[3] = div255(vmull_u8(a, t3));
			px8_over(d + i * 4, p);
		}
	} else if (k->kind == TEXEL_RGB) {
		bool opaque = !k->tinted || t[3] == 255;
		for (; i + 8 <= n; i += 8) {
			uint8x8x3_t rgb = vld3_u8(src + (unsigned)i * 3);
			uint8x8x4_t p = { { rgb.val[0], rgb.val[1], rgb.val[2], vdup_n_u8(255) } };
			if (k->tinted) {
				p.val[0] = div255(vmull_u8(p.val[0], t0));
				p.val[1] = div255(vmull_u8(p.val[1], t1));
				p.val[2] = div255(vmull_u8(p.val[2], t2));
				p.val[3] = t3;
			}
			if (opaque)
				vst4_u8(d + i * 4, p);
			else
				px8_over(d + i * 4, p);
		}
	} else {
		bool straight = k->kind == TEXEL_STRAIGHT;
		for (; i + 8 <= n; i += 8) {
			uint8x8x4_t p = vld4_u8(src + (unsigned)i * 4);
			uint64_t all = vget_lane_u64(vreinterpret_u64_u8(p.val[3]), 0);
			if (!all)
				continue;
			if (all == ~0ull && !k->tinted) {
				/* Opaque, so that premultiplying changes nothing. */
				vst4_u8(d + i * 4, p);
				continue;
			}
			if (straight)
				for (int c = 0; c < 3; c++)
					p.val[c] = div255(vmull_u8(p.val[c], p.val[3]));
			if (k->tinted) {
				p.val[0] = div255(vmull_u8(p.val[0], t0));
				p.val[1] = div255(vmull_u8(p.val[1], t1));
				p.val[2] = div255(vmull_u8(p.val[2], t2));
				p.val[3] = div255(vmull_u8(p.val[3], t3));
			}
			px8_over(d + i * 4, p);
		}
	}
#endif
	for (; i < n; i++) {
		uint8_t p[4];
		if (texel_px(k, src + (unsigned)i * bytes, p))
			px_over(d + i * 4, p);
	}
}

/* The texels src for the pixels x0 to x1 of the row y, as far as the
 * rounded clip leaves them. */
static void texture_span(int y, int x0, int x1, const uint8_t *src, const struct texels *k)
{
	uint8_t *row = paint.px + (uint32_t)y * paint.pitch;
	struct arc a;
	int w0, w1;
	bool cut = clip_arc(y, &a, &w0, &w1);
	if (w0 < x0)
		w0 = x0;
	if (w1 > x1)
		w1 = x1;
	if (w0 < w1)
		texture_row(row + (uint32_t)w0 * 4, src + (unsigned)(w0 - x0) * k->bytes, w1 - w0, k);
	else
		w0 = w1 = x1;
	if (!cut)
		return;
	for (int x = x0 == w0 ? w1 : x0; x < x1; x = x + 1 == w0 ? w1 : x + 1) {
		uint8_t p[4];
		if (texel_px(k, src + (unsigned)(x - x0) * k->bytes, p))
			px_cover(row + (uint32_t)x * 4, p, clip_cover(&a, x));
	}
}

/* The widest run of texels a scaled row is gathered into. */
#define ROW_MAX	512

/* A texture at another size takes the nearest texel for each pixel, as
 * the UI does itself; a glyph's coverage would want better, and is left
 * to the UI. */
bool soft_paint_texture(const uint8_t *data, uint32_t stride, uint32_t width, uint32_t height, int format, int dx, int dy,
			int dw, int dh, const int32_t clip[4], uint32_t tint, bool colorize)
{
	bool scaled = dw != (int)width || dh != (int)height;
	if (scaled && (format == PAINT_ALPHA || dw <= 0 || dh <= 0 || !width || !height || width > 32767 || height > 32767))
		return false;
	int x0 = dx, y0 = dy, x1 = dx + dw, y1 = dy + dh;
	if (!paint_clip(&x0, &y0, &x1, &y1, clip))
		return true;
	if (scaled && x1 - x0 > ROW_MAX)
		return false;
	struct texels k = {
		.bytes = format == PAINT_ALPHA ? 1 : format == PAINT_RGB ? 3 : 4,
		.kind = format == PAINT_ALPHA || colorize ? TEXEL_ALPHA :
			format == PAINT_RGBA_PREMULTIPLIED ? TEXEL_PREMULTIPLIED :
			format == PAINT_RGBA ? TEXEL_STRAIGHT : TEXEL_RGB,
		.tint = { (uint8_t)tint, (uint8_t)(tint >> 8), (uint8_t)(tint >> 16), (uint8_t)(tint >> 24) },
		.tinted = tint != 0xffffffffu,
	};
	/* Rounded up, so that a whole number of pixels to a texel comes out
	 * as that. */
	uint32_t step_x = scaled ? ((width << 16) + (uint32_t)dw - 1) / (uint32_t)dw : 0;
	uint32_t step_y = scaled ? ((height << 16) + (uint32_t)dh - 1) / (uint32_t)dh : 0;
	uint8_t gathered[ROW_MAX * 4];
	uint32_t shown = ~0u;
	for (int y = y0; y < y1; y++) {
		if (!scaled) {
			texture_span(y, x0, x1, data + (uint32_t)(y - dy) * stride + (uint32_t)(x0 - dx) * k.bytes, &k);
			continue;
		}
		uint32_t sy = ((uint32_t)(y - dy) * step_y) >> 16;
		if (sy >= height)
			sy = height - 1;
		/* The texels are the row before's when the source row is. */
		if (sy != shown) {
			shown = sy;
			const uint8_t *line = data + sy * stride;
			uint32_t pos = (uint32_t)(x0 - dx) * step_x, last = width - 1;
			int n = x1 - x0;
			/* Sized by the texel, so that each is a load and a store and not a call. */
			if (k.bytes == 4) {
				for (int i = 0; i < n; i++, pos += step_x) {
					uint32_t sx = pos >> 16;
					__builtin_memcpy(gathered + i * 4, line + (sx < width ? sx : last) * 4, 4);
				}
			} else if (k.bytes == 3) {
				for (int i = 0; i < n; i++, pos += step_x) {
					uint32_t sx = pos >> 16;
					const uint8_t *q = line + (sx < width ? sx : last) * 3;
					gathered[i * 3] = q[0];
					gathered[i * 3 + 1] = q[1];
					gathered[i * 3 + 2] = q[2];
				}
			} else {
				for (int i = 0; i < n; i++, pos += step_x) {
					uint32_t sx = pos >> 16;
					gathered[i] = line[sx < width ? sx : last];
				}
			}
		}
		texture_span(y, x0, x1, gathered, &k);
	}
	return true;
}

/* A filled rectangle with rounded corners and no border: each row is a
 * run of whole pixels with, where a corner's arc passes, the pixels it
 * cuts on either side. One with a border is left to the UI. */
bool soft_paint_rounded(float fx0, float fy0, float fx1, float fy1, const float radii[4], float border, const int32_t clip[4],
			uint32_t fill_color, uint32_t edge)
{
	if (border > 0.0f)
		return false;
	int x0 = round_px(fx0), y0 = round_px(fy0), x1 = round_px(fx1), y1 = round_px(fy1);
	int cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;
	if (x1 <= x0 || y1 <= y0 || !paint_clip(&cx0, &cy0, &cx1, &cy1, clip) || !fill_color)
		return true;
	float w = (float)(x1 - x0), h = (float)(y1 - y0), most = (w < h ? w : h) * 0.5f, r[4];
	for (int k = 0; k < 4; k++)
		r[k] = radii[k] < 0.0f ? 0.0f : radii[k] > most ? most : radii[k];
	uint8_t s[4] = { (uint8_t)fill_color, (uint8_t)(fill_color >> 8), (uint8_t)(fill_color >> 16),
			 (uint8_t)(fill_color >> 24) };
	for (int y = cy0; y < cy1; y++) {
		uint8_t *row = paint.px + (uint32_t)y * paint.pitch;
		float yc = (float)(y - y0) + 0.5f;
		/* The corner this row is in on each side, if any. */
		float rl = yc < r[0] ? r[0] : yc > h - r[3] ? r[3] : 0.0f, dl = yc < r[0] ? r[0] - yc : yc - (h - r[3]);
		float rr = yc < r[1] ? r[1] : yc > h - r[2] ? r[2] : 0.0f, dr = yc < r[1] ? r[1] - yc : yc - (h - r[2]);
		struct arc left = { 0 }, right = { 0 }, cut;
		int w0, w1;
		bool clipped = clip_arc(y, &cut, &w0, &w1);
		if (rl > 0.0f)
			arc_set(&left, rl, dl);
		if (rr > 0.0f)
			arc_set(&right, rr, dr);
		for (int side = 0; side < 2; side++) {
			const struct arc *a = side ? &right : &left;
			for (int i = a->lo; i < a->hi; i++) {
				int x = side ? x1 - 1 - i : x0 + i;
				if (x < cx0 || x >= cx1)
					continue;
				uint32_t cover = arc_cover(a, i);
				if (clipped)
					cover = scale8(cover, clip_cover(&cut, x));
				px_cover(row + (uint32_t)x * 4, s, cover);
			}
		}
		int s0 = x0 + left.hi, s1 = x1 - right.hi;
		if (s0 < cx0)
			s0 = cx0;
		if (s1 > cx1)
			s1 = cx1;
		if (s0 < s1)
			rect_span(y, s0, s1, fill_color, false);
	}
	return true;
}
void soft_paint_cpu(uint32_t y0, uint32_t y1) {}
void soft_paint_sync(void) {}
void soft_paint_end(void) {}
void soft_paint_finish(void) {}
