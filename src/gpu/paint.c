/* The UI's drawing on the GPU. Everything is a texel times a colour, so
 * consecutive items batch into one draw until the texture or the blend
 * changes. Glyphs, alpha maps and the corners of rounded rectangles live
 * in an 8-bit atlas whose first texels are white, which plain rectangles
 * sample; images live in an RGBA atlas, premultiplied on the way in.
 * Texels drawn at their own size are sampled as they are, scaled ones
 * filtered linearly.
 * Both are filled shelf by shelf in uncached memory and both start over
 * when either is full. A rounded rectangle is its corners, drawn from
 * coverage masks made once per radius and border, and plain rectangles
 * for the rest. The layer is cached memory: rows the CPU wrote are
 * cleaned before the GPU blends over them, and the rows the GPU drew are
 * invalidated once it is done. paint_end does not wait for that: the
 * rows are noted and invalidated by paint_finish, or by the next wait,
 * and the vertices of submissions in flight are kept until then. */
#include <string.h>
#include "asteroid.h"
#include "a3xx.h"
#include "draw.h"
#include "paint.h"
#include "shaders.h"

#define ALPHA_W		1024
#define ALPHA_H		1024
#define IMAGE_W		512
#define IMAGE_H		512
#define SLOTS		2048
#define VERT_FLOATS	10
#define VERTS		(6 * 4096)
#define MAX_RADIUS	96
#define CORNER_KEY	0xf0000000u

enum { NONE = -1, TEX_ALPHA, TEX_IMAGE, TEX_IMAGE_ALPHA, TEX_LINEAR = 4 };

struct atlas {
	uint8_t *texels;
	uint32_t width, height, bpp, x, y, row_h;
	bool changed;
};

static struct atlas atlases[2] = {
	{ .width = ALPHA_W, .height = ALPHA_H, .bpp = 1 },
	{ .width = IMAGE_W, .height = IMAGE_H, .bpp = 4 },
};

static struct slot {
	const uint8_t *key;
	uint16_t x, y, w, h;
	uint8_t atlas;
} slots[SLOTS];

static struct gpu_target target;
static uint32_t *layer;
static float *verts;
static unsigned nverts, first;
static int kind = NONE;
static bool copy_mode, begun, drawn, ready;
static int dirty_y0, dirty_y1;
static struct { uint32_t *layer; uint32_t pitch; int y0, y1; } pending[8];
static unsigned npending;

static bool setup(void)
{
	if (ready)
		return true;
	for (int i = 0; i < 2; i++) {
		struct atlas *a = &atlases[i];
		a->texels = gpu_alloc(a->width * a->height * a->bpp, 4096);
		if (!a->texels)
			return false;
	}
	verts = gpu_alloc(VERTS * VERT_FLOATS * 4, 64);
	if (!verts)
		return false;
	ready = true;
	return true;
}

static void reset(void)
{
	memset(slots, 0, sizeof slots);
	for (int i = 0; i < 2; i++)
		atlases[i].x = atlases[i].y = atlases[i].row_h = 0;
	struct atlas *a = &atlases[0];
	for (int r = 0; r < 2; r++)
		memset(a->texels + r * a->width, 0xff, 2);
	a->x = 3;
	a->row_h = 2;
}

static void flush(void)
{
	if (nverts == first)
		return;
	if (!begun) {
		gpu_draw_begin(&target);
		gpu_draw_program(&shader_f_vs, &shader_f_fs);
		begun = true;
		drawn = false;
		for (int i = 0; i < 2; i++)
			atlases[i].changed = false;
	}
	int t = kind & ~TEX_LINEAR;
	struct atlas *a = &atlases[t == TEX_ALPHA ? 0 : 1];
	if (drawn && a->changed)
		gpu_draw_invalidate_textures();
	a->changed = false;
	gpu_draw_texture_format(0, (uint32_t)(uintptr_t)a->texels, a->width, a->height, a->width * a->bpp,
				(t == TEX_ALPHA ? GPU_TEX_ALPHA : t == TEX_IMAGE ? GPU_TEX_RGBA : GPU_TEX_RGBA_ALPHA) |
				(kind & TEX_LINEAR ? GPU_TEX_LINEAR : 0));
	gpu_draw_blend(!copy_mode);
	gpu_draw_triangles((uint32_t)(uintptr_t)(verts + first * VERT_FLOATS), VERT_FLOATS * 4, 3,
			   (const uint8_t[]){ 4, 2, 4 }, nverts - first);
	drawn = true;
	first = nverts;
}

/* Ends the layer's submission and notes the rows it draws. */
static void submit(void)
{
	flush();
	if (begun)
		gpu_draw_submit();
	begun = false;
	if (dirty_y0 < dirty_y1)
		pending[npending++] = (typeof(pending[0])){ layer, target.pitch_bytes, dirty_y0, dirty_y1 };
	dirty_y0 = (int)target.height;
	dirty_y1 = 0;
}

void paint_finish(void)
{
	if (!npending && nverts == 0)
		return;
	if (!gpu_draw_wait())
		log_printf("paint: draw did not complete\n");
	for (unsigned i = 0; i < npending; i++)
		dcache_invalidate_range(pending[i].layer + pending[i].y0 * (pending[i].pitch / 4),
					(uint32_t)(pending[i].y1 - pending[i].y0) * pending[i].pitch);
	npending = 0;
	nverts = first = 0;
}

void paint_sync(void)
{
	submit();
	paint_finish();
}

bool paint_begin(uint32_t *buf, uint32_t width, uint32_t height, uint32_t stride_px)
{
	if (!gpu_ready() || !setup())
		return false;
	if (npending == ARRAY_SIZE(pending))
		paint_finish();
	if (!atlases[0].row_h)
		reset();
	layer = buf;
	target = (struct gpu_target){ (uint32_t)(uintptr_t)buf, width, height, stride_px * 4, true };
	kind = NONE;
	dirty_y0 = (int)height;
	dirty_y1 = 0;
	return true;
}

void paint_end(void)
{
	submit();
	layer = NULL;
}

void paint_cpu(uint32_t y0, uint32_t y1)
{
	if (layer && y0 < y1)
		dcache_clean_invalidate_range(layer + y0 * (target.pitch_bytes / 4), (y1 - y0) * target.pitch_bytes);
}

/* Switches to texture k and the blend with room for n more quads. */
static void use(int k, bool copy, unsigned n)
{
	if (k == kind && copy == copy_mode && nverts + 6 * n <= VERTS)
		return;
	flush();
	if (nverts + 6 * n > VERTS)
		paint_sync();
	kind = k;
	copy_mode = copy;
}

/* One quad: (x0, y0)-(x1, y1) in layer pixels cut to the clip, with the
 * texture coordinates following the cut. */
static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const int32_t clip[4],
		 uint32_t color)
{
	float nx0 = x0 > (float)clip[0] ? x0 : (float)clip[0], ny0 = y0 > (float)clip[1] ? y0 : (float)clip[1];
	float nx1 = x1 < (float)clip[2] ? x1 : (float)clip[2], ny1 = y1 < (float)clip[3] ? y1 : (float)clip[3];
	if (nx0 >= nx1 || ny0 >= ny1)
		return;
	float du = (u1 - u0) / (x1 - x0), dv = (v1 - v0) / (y1 - y0);
	u1 = u0 + (nx1 - x0) * du;
	u0 = u0 + (nx0 - x0) * du;
	v1 = v0 + (ny1 - y0) * dv;
	v0 = v0 + (ny0 - y0) * dv;
	float w = (float)target.width, h = (float)target.height;
	float r = (float)(color & 0xff) / 255.0f, g = (float)((color >> 8) & 0xff) / 255.0f;
	float b = (float)((color >> 16) & 0xff) / 255.0f, a = (float)(color >> 24) / 255.0f;
	const float c[6][4] = {
		{ nx0, ny0, u0, v0 }, { nx1, ny0, u1, v0 }, { nx0, ny1, u0, v1 },
		{ nx1, ny0, u1, v0 }, { nx1, ny1, u1, v1 }, { nx0, ny1, u0, v1 },
	};
	float *o = verts + nverts * VERT_FLOATS;
	for (int i = 0; i < 6; i++, o += VERT_FLOATS) {
		o[0] = 2.0f * c[i][0] / w - 1.0f;
		o[1] = 1.0f - 2.0f * c[i][1] / h;
		o[2] = 0.0f;
		o[3] = 1.0f;
		o[4] = c[i][2];
		o[5] = c[i][3];
		o[6] = r;
		o[7] = g;
		o[8] = b;
		o[9] = a;
	}
	nverts += 6;
	if ((int)ny0 < dirty_y0)
		dirty_y0 = (int)ny0;
	if ((int)(ny1 + 0.999f) > dirty_y1)
		dirty_y1 = (int)(ny1 + 0.999f);
}

static void solid(float x0, float y0, float x1, float y1, const int32_t clip[4], uint32_t color)
{
	float u = 1.0f / ALPHA_W, v = 1.0f / ALPHA_H;
	quad(x0, y0, x1, y1, u, v, u, v, clip, color);
}

void paint_rect(float x0, float y0, float x1, float y1, const int32_t clip[4], uint32_t color, bool copy)
{
	if (!layer)
		return;
	use(TEX_ALPHA, copy, 1);
	solid(x0, y0, x1, y1, clip, color);
}

static struct slot *find(const uint8_t *key, uint32_t w, uint32_t h, unsigned atlas, bool *found)
{
	unsigned i = (unsigned)((uintptr_t)key >> 2) * 2654435761u % SLOTS;
	for (unsigned n = 0; n < SLOTS; n++, i = (i + 1) % SLOTS) {
		struct slot *s = &slots[i];
		if (!s->key) {
			*found = false;
			return s;
		}
		if (s->key == key && s->w == w && s->h == h && s->atlas == atlas) {
			*found = true;
			return s;
		}
	}
	return NULL;
}

static bool place(struct atlas *a, uint32_t w, uint32_t h, uint32_t *x, uint32_t *y)
{
	if (a->x + w > a->width) {
		a->y += a->row_h + 1;
		a->x = 0;
		a->row_h = 0;
	}
	if (w > a->width || a->y + h > a->height)
		return false;
	*x = a->x;
	*y = a->y;
	a->x += w + 1;
	if (h > a->row_h)
		a->row_h = h;
	return true;
}

static void copy_in(struct atlas *a, uint32_t x, uint32_t y, const uint8_t *data, uint32_t stride, uint32_t w,
		    uint32_t h, int format)
{
	for (uint32_t r = 0; r < h; r++) {
		const uint8_t *s = data + r * stride;
		uint8_t *d = a->texels + ((y + r) * a->width + x) * a->bpp;
		if (format == PAINT_ALPHA || format == PAINT_RGBA_PREMULTIPLIED) {
			memcpy(d, s, w * a->bpp);
			continue;
		}
		unsigned n = format == PAINT_RGB ? 3 : 4;
		for (uint32_t c = 0; c < w; c++, d += 4) {
			uint32_t al = format == PAINT_RGB ? 255 : s[c * 4 + 3];
			d[0] = (uint8_t)(s[c * n] * al / 255);
			d[1] = (uint8_t)(s[c * n + 1] * al / 255);
			d[2] = (uint8_t)(s[c * n + 2] * al / 255);
			d[3] = (uint8_t)al;
		}
	}
	a->changed = true;
}

/* The atlas slot holding the texels for key, copied in from data (or
 * made by make) on first use; the atlases start over, once the GPU is
 * done with them, when there is no room. */
static struct slot *lookup(const uint8_t *key, const uint8_t *data, uint32_t stride, uint32_t width, uint32_t height,
			   int format, void (*make)(uint8_t *out, uint32_t key))
{
	unsigned n = format == PAINT_ALPHA ? 0 : 1;
	struct atlas *a = &atlases[n];
	bool found;
	struct slot *s = find(key, width, height, n, &found);
	if (found)
		return s;
	uint32_t x, y;
	if (!s || !place(a, width, height, &x, &y)) {
		paint_sync();
		reset();
		s = find(key, width, height, n, &found);
		if (!s || !place(a, width, height, &x, &y))
			return NULL;
	}
	if (make) {
		static uint8_t mask[MAX_RADIUS * MAX_RADIUS];
		make(mask, (uint32_t)(uintptr_t)key);
		data = mask;
		stride = width;
	}
	copy_in(a, x, y, data, stride, width, height, format);
	*s = (struct slot){ key, (uint16_t)x, (uint16_t)y, (uint16_t)width, (uint16_t)height, (uint8_t)n };
	return s;
}

bool paint_texture(const uint8_t *data, uint32_t stride, uint32_t width, uint32_t height, int format, int dx, int dy,
		   int dw, int dh, const int32_t clip[4], uint32_t tint, bool colorize)
{
	if (!layer || format < PAINT_RGB || format > PAINT_ALPHA || !width || !height || dw <= 0 || dh <= 0)
		return false;
	struct slot *s = lookup(data, data, stride, width, height, format, NULL);
	if (!s)
		return false;
	struct atlas *a = &atlases[s->atlas];
	int k = s->atlas == 0 ? TEX_ALPHA : colorize ? TEX_IMAGE_ALPHA : TEX_IMAGE;
	use(k | ((uint32_t)dw != width || (uint32_t)dh != height ? TEX_LINEAR : 0), false, 1);
	quad((float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh), (float)s->x / a->width, (float)s->y / a->height,
	     (float)(s->x + s->w) / a->width, (float)(s->y + s->h) / a->height, clip, tint);
	return true;
}

static float root(float x)
{
	float r;
	__asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
	return r;
}

/* Coverage of a top-left corner of radius r (bits 8-15 of the key) with
 * a border of width b (bits 0-7): the area inside the border, or with
 * bit 16 set the border itself. */
static void corner(uint8_t *out, uint32_t key)
{
	int r = (key >> 8) & 0xff, b = key & 0xff;
	bool ring = key & 0x10000;
	for (int y = 0; y < r; y++)
		for (int x = 0; x < r; x++) {
			float dx = (float)r - (x + 0.5f), dy = (float)r - (y + 0.5f), d = root(dx * dx + dy * dy);
			float outer = 0.5f - (d - (float)r), inner = 0.5f - (d - (float)(r - b));
			outer = outer < 0.0f ? 0.0f : outer > 1.0f ? 1.0f : outer;
			inner = inner < 0.0f ? 0.0f : inner > 1.0f ? 1.0f : inner;
			out[y * r + x] = (uint8_t)((ring ? outer - inner : inner) * 255.0f + 0.5f);
		}
}

static int max3(int a, int b, int c)
{
	return a > b ? (a > c ? a : c) : (b > c ? b : c);
}

bool paint_rounded(float x0, float y0, float x1, float y1, const float radii[4], float border, const int32_t clip[4],
		   uint32_t fill, uint32_t edge)
{
	if (!layer)
		return false;
	int r[4], b = (int)border;
	float half = (x1 - x0 < y1 - y0 ? x1 - x0 : y1 - y0) / 2.0f;
	for (int i = 0; i < 4; i++) {
		r[i] = (int)(radii[i] < half ? radii[i] : half);
		if (r[i] > MAX_RADIUS)
			return false;
		if (r[i] <= b)
			r[i] = 0;
	}
	/* Corners in the order of the radii, top left, top right, bottom
	 * right and bottom left, each the top-left mask flipped into place. */
	const float cx[4] = { x0, x1, x1, x0 }, cy[4] = { y0, y0, y1, y1 };
	const bool flip_x[4] = { false, true, true, false }, flip_y[4] = { false, false, true, true };
	for (int i = 0; i < 4; i++) {
		if (!r[i])
			continue;
		for (int ring = 0; ring < (b ? 2 : 1); ring++) {
			uint32_t key = CORNER_KEY | (uint32_t)ring << 16 | (uint32_t)r[i] << 8 | (uint32_t)b;
			struct slot *s = lookup((const uint8_t *)(uintptr_t)key, NULL, 0, r[i], r[i], PAINT_ALPHA, corner);
			if (!s)
				return false;
			use(TEX_ALPHA, false, 1);
			float u0 = (float)s->x / ALPHA_W, v0 = (float)s->y / ALPHA_H;
			float u1 = (float)(s->x + r[i]) / ALPHA_W, v1 = (float)(s->y + r[i]) / ALPHA_H;
			float qx0 = flip_x[i] ? cx[i] - r[i] : cx[i], qy0 = flip_y[i] ? cy[i] - r[i] : cy[i];
			quad(qx0, qy0, qx0 + r[i], qy0 + r[i], flip_x[i] ? u1 : u0, flip_y[i] ? v1 : v0, flip_x[i] ? u0 : u1,
			     flip_y[i] ? v0 : v1, clip, ring ? edge : fill);
		}
	}
	/* The fill inside the border, in a band along the top, the middle and
	 * a band along the bottom, then the straight parts of the border. */
	float bf = (float)b, tl = (float)(r[0] > b ? r[0] : b), tr = (float)(r[1] > b ? r[1] : b);
	float br = (float)(r[2] > b ? r[2] : b), bl = (float)(r[3] > b ? r[3] : b);
	float top = (float)max3(r[0], r[1], b), bottom = (float)max3(r[2], r[3], b);
	use(TEX_ALPHA, false, 13);
	solid(x0 + tl, y0 + bf, x1 - tr, y0 + top, clip, fill);
	solid(x0 + bf, y0 + tl, x0 + tl, y0 + top, clip, fill);
	solid(x1 - tr, y0 + tr, x1 - bf, y0 + top, clip, fill);
	solid(x0 + bf, y0 + top, x1 - bf, y1 - bottom, clip, fill);
	solid(x0 + bl, y1 - bottom, x1 - br, y1 - bf, clip, fill);
	solid(x0 + bf, y1 - bottom, x0 + bl, y1 - bl, clip, fill);
	solid(x1 - br, y1 - bottom, x1 - bf, y1 - br, clip, fill);
	if (b) {
		solid(x0 + r[0], y0, x1 - r[1], y0 + bf, clip, edge);
		solid(x0 + r[3], y1 - bf, x1 - r[2], y1, clip, edge);
		solid(x0, y0 + tl, x0 + bf, y1 - bl, clip, edge);
		solid(x1 - bf, y0 + tr, x1, y1 - br, clip, edge);
	}
	return true;
}
