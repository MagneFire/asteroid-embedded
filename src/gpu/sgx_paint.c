/* The UI's painting of paint.h on minnow: on the SGX530 (sgx_draw.c)
 * with the programs its scene has, or on the CPU (soft.c) while the GPU
 * is off, for repaints too small to pay for a scene, and for any call the
 * GPU cannot paint.
 *
 * A scene draws into the layer itself, and the GPU clears its whole
 * target and stores every tile of it. So after the flat draw the scene
 * must start with, the layer is drawn onto itself, texel for pixel,
 * everywhere but where the repaint's background goes: Slint first fills
 * the dirty region, as paint_rect with copy, and those rectangles stay at
 * the clear's 0, over which blending the copy's colour gives the colour.
 * Everything else is quads of texels times a colour per vertex, from one
 * RGBA atlas sampled nearest at texel centres: images, premultiplied on
 * the way in; glyphs and alpha maps as white of their coverage; a white
 * texel for plain colours; and the corners of rounded rectangles as
 * soft.c's coverage. So texels go in the atlas once whatever their
 * colour, and all quads of the atlas batch into one draw.
 *
 * The layer's bytes are R, G, B, A; with the rows format 0x0c a texel's
 * bytes land in the target as they are, so the atlas holds the layer's
 * own byte order, and only the vertices' colours are swapped.
 *
 * A call the GPU cannot paint ends the scene; the CPU paints the call
 * and the next scene's self-copy keeps it. A scene renders on until the
 * next scene, a CPU write to its layer or the compositor waits for it;
 * the rows it changed are then invalidated, and rows the CPU wrote are
 * cleaned before the next. A scene is kept as its calls, so
 * that when the GPU refuses or fails it the CPU paints it again; after a
 * failure the GPU is halted and the CPU paints until sgx paint gpu. */
#include <string.h>
#include "asteroid.h"
#include "gpu/a3xx.h"
#include "gpu/paint.h"
#include "gpu/soft.h"
#include "gpu/sgx.h"
#include "gpu/sgx_draw.h"
#include "gpu/sgx_scene_data.h"

#ifdef HOST_TEST
extern uintptr_t gpu_host_base;
#define PHYS(p)		((uint32_t)((uintptr_t)(p) - gpu_host_base))
#else
#define PHYS(p)		((uint32_t)(uintptr_t)(p))
#endif

#define SIZE		SGX_DRAW_SIZE
#define PITCH		(SIZE * 4)
/* The atlases are allocated atlas_w x atlas_h, or the small size if
 * that does not fit. They grow in height: the GPU sampled the rows of
 * a 960 wide one wrong, 480 wide ones of 928 rows right. */
#define SMALL_W		480
#define SMALL_H		480
#define WIDE_MAX	511
#define SLOTS		2048
#define MAX_COPY	8
#define MAX_OPS		256
#define MAX_QUADS	9
#define QUAD_BYTES	(6 * 16)
#define CQUAD_BYTES	(6 * 32)
#define WHITE		0xffffffffu
#define DRAW_QUADS	(SGX_DRAW_MAX_VERTS / 6)
#define FLAT_BYTES	80
/* Below this many pixels of background a repaint costs the CPU less
 * than a scene, whose self-copy alone renders the whole layer. */
#define SMALL_PX	(64 * 64)
#define ROWS_PAINT	0x0c
#define ROWS_COMPOSE	0x0d

enum { K_COLOR, K_IMAGE, K_ALPHA, K_CORNER };
enum { FLUSH_END, FLUSH_SYNC, FLUSH_ROOM, FLUSH_COPY, FLUSH_ATLAS, FLUSH_CPU, FLUSHES };
enum { CPU_CLIP, CPU_SCALED_ALPHA, CPU_BORDER, CPU_BIG, CPU_FORMAT, CPU_COPY_CLIP, CPU_REFUSED, CPU_FAILED, CPUS };
enum { OP_RECT, OP_TEXTURE, OP_ROUNDED, OP_CLIP };

/* Texels to find or put in the atlas-> */
struct src {
	uint8_t kind, format;
	const uint8_t *data;
	uint32_t stride, w, h, tint, sum;
	float r;
};

struct slot {
	const void *key;
	uint32_t sum, tint;
	uint16_t x, y, w, h;
	uint8_t kind, format;
	bool used;
};

/* Layer pixels and the texels of src across them, from the slot's
 * corner. */
struct quad {
	int x0, y0, x1, y1;
	float u0, v0, u1, v1;
	uint8_t src;
};

/* A call, kept for the CPU to paint again. */
struct op {
	uint8_t op;
	int8_t format;
	bool flag;
	const uint8_t *data;
	uint32_t stride, width, height, c0, c1;
	int32_t clip[4];
	int dx, dy, dw, dh;
	float f[5], radii[4], border;
};

static int mode = PAINT_MODE_SOFT;
static bool dead, told;

/* Two with overlap, so that starting over does not wait for the scene
 * that samples the one in use. */
static struct atlas {
	uint8_t *texels;
	uint32_t gpu, gen, x, y, row_h, w, h;
	struct slot slots[SLOTS];
	unsigned nslots;
} atlases[2], *atlas = &atlases[0];
static uint32_t atlas_epoch, atlas_w = 480, atlas_h = 928;

/* What each layer's repaints drew since paint_bounds last asked, by
 * band of PAINT_BAND rows: the columns that may hold content now, the
 * columns a background replaced in all of the band's rows, and the
 * bands with rows the UI wrote itself, which only a scan knows. */
#define BOUNDS 8
static struct bounds {
	const uint32_t *layer;
	int16_t x0[PAINT_BANDS], x1[PAINT_BANDS], c0[PAINT_BANDS], c1[PAINT_BANDS];
	uint32_t scan;
} bounds[BOUNDS], *bnd;

/* The repaint. */
static struct {
	bool gpu, decided, rclip;
	uint32_t *layer;
	uint32_t layer_gpu;
	int cpu_y0, cpu_y1;
	int rx0, ry0, rx1, ry1;
	float radius, clip[5];
} rep;

/* The scene: while head, only the background's rectangles are known and
 * nothing is written for the GPU yet. */
struct scene {
	bool open, head, broken, atlas_used, rclip;
	unsigned ncopy, nd, nops;
	struct { int x0, y0, x1, y1; uint32_t color; } copy[MAX_COPY];
	struct sgx_draw draws[SGX_DRAW_MAX_DRAWS];
	uint32_t vbytes;
	int y0, y1;
	float clip[5];
	struct op ops[MAX_OPS];
};
/* With overlap the scene last kicked renders while the next one is
 * built in the other record; it is kept to be painted again should the
 * GPU fail it. */
static struct scene scenes[2], *sc = &scenes[0];
static struct {
	bool busy;
	uint32_t *layer, ticket;
	struct scene *s;
	struct atlas *atlas;
} fl;
static bool overlap = true;

static struct {
	uint32_t repaints, small, off, scenes, ta_us, render_us, wait_us, quads, uploads, upload_px, restarts, switches;
	uint32_t rects, textures, rounded, flush[FLUSHES], cpu[CPUS];
} st;

static void flush_scene(int why);
static bool settle(void);

void paint_mode(int m)
{
	settle();
	mode = m;
	paint_null(m == PAINT_MODE_NULL);
	if (m == PAINT_MODE_GPU)
		dead = told = false;
}

int paint_mode_now(void) { return mode; }

bool paint_gpu_lost(void) { return dead; }

static inline uint32_t scale8(uint32_t v, uint32_t by)
{
	uint32_t t = v * by + 128;
	return (t + (t >> 8)) >> 8;
}

static int round_px(float v) { return (int)(v < 0.0f ? v - 0.5f : v + 0.5f); }

static int ceil_px(float v)
{
	int i = (int)v;
	return (float)i < v ? i + 1 : i;
}

static int floor_px(float v)
{
	int i = (int)v;
	return (float)i > v ? i - 1 : i;
}

static int min_i(int a, int b) { return a < b ? a : b; }
static int max_i(int a, int b) { return a > b ? a : b; }

static void add_rows(int *lo, int *hi, int y0, int y1)
{
	y0 = max_i(y0, 0);
	y1 = min_i(y1, SIZE);
	if (y0 >= y1)
		return;
	*lo = min_i(*lo, y0);
	*hi = max_i(*hi, y1);
}

static void clean_rows(uint32_t *layer, int y0, int y1)
{
	if (y0 < y1)
		dcache_clean_range(layer + y0 * SIZE, (uint32_t)(y1 - y0) * PITCH);
}

static void invalidate_rows(uint32_t *layer, int y0, int y1)
{
	if (y0 < y1)
		dcache_invalidate_range(layer + y0 * SIZE, (uint32_t)(y1 - y0) * PITCH);
}

/* As soft.c clips: to the clip, the layer and the rounded clip's box. */
static void note(const int r[4])
{
	if (!bnd || r[0] >= r[2] || r[1] >= r[3])
		return;
	for (int b = r[1] / PAINT_BAND; b <= (r[3] - 1) / PAINT_BAND && b < PAINT_BANDS; b++) {
		if (r[0] < bnd->x0[b])
			bnd->x0[b] = (int16_t)r[0];
		if (r[2] > bnd->x1[b])
			bnd->x1[b] = (int16_t)r[2];
	}
}

/* A background over r: what it covers in whole bands is replaced. */
static void note_clear(const int r[4])
{
	if (!bnd || r[0] >= r[2] || r[1] >= r[3])
		return;
	for (int b = (r[1] + PAINT_BAND - 1) / PAINT_BAND; (b + 1) * PAINT_BAND <= r[3] && b < PAINT_BANDS; b++) {
		bool joins = bnd->c0[b] < bnd->c1[b] && r[0] <= bnd->c1[b] && r[2] >= bnd->c0[b];
		if (joins || r[2] - r[0] > bnd->c1[b] - bnd->c0[b]) {
			bnd->c0[b] = (int16_t)(joins && bnd->c0[b] < r[0] ? bnd->c0[b] : r[0]);
			bnd->c1[b] = (int16_t)(joins && bnd->c1[b] > r[2] ? bnd->c1[b] : r[2]);
		}
	}
}

static void note_rows(uint32_t y0, uint32_t y1)
{
	if (!bnd)
		return;
	for (uint32_t b = y0 / PAINT_BAND; b < PAINT_BANDS && b * PAINT_BAND < y1; b++)
		bnd->scan |= 1u << b;
}

static void bounds_begin(const uint32_t *layer)
{
	struct bounds *free = NULL;
	for (bnd = bounds; bnd < bounds + BOUNDS; bnd++) {
		if (bnd->layer == layer)
			return;
		if (!bnd->layer && !free)
			free = bnd;
	}
	bnd = free;
	if (!bnd)
		return;
	bnd->layer = layer;
	bnd->scan = 0;
	for (int b = 0; b < PAINT_BANDS; b++) {
		bnd->x0[b] = bnd->c0[b] = SIZE;
		bnd->x1[b] = bnd->c1[b] = 0;
	}
}

bool paint_bounds(const uint32_t *layer, struct paint_bounds *out)
{
	for (struct bounds *b = bounds; b < bounds + BOUNDS; b++) {
		if (b->layer != layer)
			continue;
		memcpy(out->x0, b->x0, sizeof b->x0);
		memcpy(out->x1, b->x1, sizeof b->x1);
		memcpy(out->c0, b->c0, sizeof b->c0);
		memcpy(out->c1, b->c1, sizeof b->c1);
		out->scan = b->scan;
		b->layer = NULL;
		if (bnd == b)
			bnd = NULL;
		return true;
	}
	return false;
}

static bool cut(int r[4], const int32_t clip[4])
{
	r[0] = max_i(max_i(r[0], clip[0]), 0);
	r[1] = max_i(max_i(r[1], clip[1]), 0);
	r[2] = min_i(min_i(r[2], clip[2]), SIZE);
	r[3] = min_i(min_i(r[3], clip[3]), SIZE);
	if (rep.rclip) {
		r[0] = max_i(r[0], rep.rx0);
		r[1] = max_i(r[1], rep.ry0);
		r[2] = min_i(r[2], rep.rx1);
		r[3] = min_i(r[3], rep.ry1);
	}
	return r[0] < r[2] && r[1] < r[3];
}

/* Whether the rounded clip leaves every pixel of r whole: soft.c cuts
 * the rows a corner's arc crosses, and in those only the pixels within
 * the radius of either side. */
static bool clear_of_corners(const int r[4])
{
	if (!rep.rclip || rep.radius <= 0.0f)
		return true;
	int rows = ceil_px(rep.radius - 0.5f), cols = ceil_px(rep.radius);
	return (r[1] >= rep.ry0 + rows && r[3] <= rep.ry1 - rows) || (r[0] >= rep.rx0 + cols && r[2] <= rep.rx1 - cols);
}

static bool clip_quad(struct quad *q, const int c[4])
{
	int x0 = max_i(q->x0, c[0]), y0 = max_i(q->y0, c[1]), x1 = min_i(q->x1, c[2]), y1 = min_i(q->y1, c[3]);
	if (x0 >= x1 || y0 >= y1)
		return false;
	float du = (q->u1 - q->u0) / (float)(q->x1 - q->x0), dv = (q->v1 - q->v0) / (float)(q->y1 - q->y0);
	float u0 = q->u0 + (float)(x0 - q->x0) * du, u1 = q->u0 + (float)(x1 - q->x0) * du;
	float v0 = q->v0 + (float)(y0 - q->y0) * dv, v1 = q->v0 + (float)(y1 - q->y0) * dv;
	*q = (struct quad){ x0, y0, x1, y1, u0, v0, u1, v1, q->src };
	return true;
}

/* --- the atlas */

static void reset_atlas(void)
{
	memset(atlas->slots, 0, sizeof atlas->slots);
	atlas->nslots = 0;
	atlas->x = atlas->y = atlas->row_h = 0;
	atlas_epoch++;
}

/* Goes on in the other atlas, which keeps what it holds, unless the
 * scene in flight samples it; false if it cannot. */
static bool switch_atlas(void)
{
	if (sc->open && sc->atlas_used)
		flush_scene(FLUSH_ATLAS);
	struct atlas *other = &atlases[atlas == &atlases[0]];
	if (!overlap || !other->texels || other->gen != sgx_generation() || (fl.busy && fl.atlas == other))
		return false;
	atlas = other;
	atlas_epoch++;
	st.switches++;
	return true;
}

/* Starts the atlas in use over, after the GPU has drawn what uses it. */
static void restart(void)
{
	if (sc->open && sc->atlas_used)
		flush_scene(FLUSH_ATLAS);
	if (fl.busy && fl.atlas == atlas)
		settle();
	reset_atlas();
	st.restarts++;
}

static bool place(uint32_t w, uint32_t h, uint32_t *x, uint32_t *y)
{
	if (atlas->x + w > atlas->w) {
		atlas->y += atlas->row_h + 1;
		atlas->x = 0;
		atlas->row_h = 0;
	}
	if (w > atlas->w || atlas->y + h > atlas->h)
		return false;
	*x = atlas->x;
	*y = atlas->y;
	atlas->x += w + 1;
	if (h > atlas->row_h)
		atlas->row_h = h;
	return true;
}

static void texel(const struct src *s, const uint8_t *q, uint32_t c, uint32_t r, const uint8_t t[4], uint8_t p[4])
{
	if (s->kind == K_COLOR) {
		__builtin_memcpy(p, t, 4);
		return;
	}
	if (s->kind == K_CORNER || s->kind == K_ALPHA) {
		uint32_t a = s->kind == K_CORNER	      ? soft_paint_corner(s->r, (int)c, (int)r) :
			     s->format == PAINT_ALPHA ? q[c] :
			     s->format == PAINT_RGB   ? 255 :
							q[c * 4 + 3];
		for (int i = 0; i < 4; i++)
			p[i] = (uint8_t)scale8(a, t[i]);
		return;
	}
	if (s->format == PAINT_RGB) {
		__builtin_memcpy(p, q + c * 3, 3);
		p[3] = 255;
	} else {
		__builtin_memcpy(p, q + c * 4, 4);
		if (s->format == PAINT_RGBA)
			for (int i = 0; i < 3; i++)
				p[i] = (uint8_t)scale8(p[i], p[3]);
	}
	if (s->tint != 0xffffffffu)
		for (int i = 0; i < 4; i++)
			p[i] = (uint8_t)scale8(p[i], t[i]);
	/* soft.c leaves out a pixel of no alpha. */
	if (!p[3])
		memset(p, 0, 4);
}

static void upload(const struct src *s, uint32_t x, uint32_t y)
{
	const uint8_t t[4] = { (uint8_t)s->tint, (uint8_t)(s->tint >> 8), (uint8_t)(s->tint >> 16),
			       (uint8_t)(s->tint >> 24) };
	/* Glyphs and premultiplied images, most of what is put in, word by
	 * word: texel() per texel is most of the cost of a repaint. The
	 * firmware is built freestanding, so memcpy is a call unless asked
	 * for as the builtin. */
	bool coverage = s->kind == K_ALPHA && s->format == PAINT_ALPHA && s->tint == WHITE;
	bool premul = s->kind == K_IMAGE && s->format == PAINT_RGBA_PREMULTIPLIED && s->tint == WHITE;
	bool straight = s->kind == K_IMAGE && s->format == PAINT_RGBA && s->tint == WHITE;
	for (uint32_t r = 0; r < s->h; r++) {
		volatile uint32_t *d = (volatile uint32_t *)(void *)(atlas->texels + ((y + r) * atlas->w + x) * 4);
		const uint8_t *q = s->data ? s->data + r * s->stride : NULL;
		if (coverage) {
			for (uint32_t c = 0; c < s->w; c++)
				d[c] = q[c] * 0x01010101u;
			continue;
		}
		if (premul) {
			for (uint32_t c = 0; c < s->w; c++) {
				uint32_t w;
				__builtin_memcpy(&w, q + c * 4, 4);
				d[c] = w >> 24 ? w : 0;
			}
			continue;
		}
		if (straight) {
			for (uint32_t c = 0; c < s->w; c++) {
				uint32_t w, a;
				__builtin_memcpy(&w, q + c * 4, 4);
				a = w >> 24;
				if (a && a != 255)
					w = scale8(w & 0xff, a) | scale8(w >> 8 & 0xff, a) << 8 | scale8(w >> 16 & 0xff, a) << 16 |
					    a << 24;
				d[c] = a ? w : 0;
			}
			continue;
		}
		for (uint32_t c = 0; c < s->w; c++) {
			uint8_t p[4];
			uint32_t w;
			texel(s, q, c, r, t, p);
			__builtin_memcpy(&w, p, 4);
			d[c] = w;
		}
	}
	st.uploads++;
	st.upload_px += s->w * s->h;
}

static const void *key_of(const struct src *s)
{
	uint32_t bits;
	if (s->kind != K_CORNER)
		return s->data;
	memcpy(&bits, &s->r, 4);
	return (const void *)(uintptr_t)bits;
}

static struct slot *find(const struct src *s, const void *key, bool *hit)
{
	uint32_t h = (uint32_t)(uintptr_t)key * 2654435761u ^ s->tint * 40503u ^ s->w << 16 ^ s->h ^ (uint32_t)s->kind << 28;
	unsigned i = h % SLOTS;
	for (unsigned n = 0; n < SLOTS; n++, i = (i + 1) % SLOTS) {
		struct slot *t = &atlas->slots[i];
		if (!t->used) {
			*hit = false;
			return t;
		}
		if (t->key == key && t->sum == s->sum && t->tint == s->tint && t->w == s->w && t->h == s->h &&
		    t->kind == s->kind && t->format == s->format) {
			*hit = true;
			return t;
		}
	}
	return NULL;
}

/* The slot holding s, put there on first use. When s does not fit, the
 * other atlas is tried before the one in use starts over. */
static const struct slot *lookup(const struct src *s)
{
	const void *key = key_of(s);
	bool switched = false;
	for (int tries = 0; tries < 3; tries++) {
		bool hit;
		uint32_t x, y;
		struct slot *t = atlas->nslots < SLOTS * 3 / 4 ? find(s, key, &hit) : NULL;
		if (t && hit)
			return t;
		if (t && place(s->w, s->h, &x, &y)) {
			*t = (struct slot){ key,	   s->sum,	   s->tint, (uint16_t)x, (uint16_t)y, (uint16_t)s->w,
					    (uint16_t)s->h, s->kind, s->format, true };
			atlas->nslots++;
			upload(s, x, y);
			return t;
		}
		if (tries == 2)
			break;
		if (!switched && switch_atlas())
			switched = true;
		else {
			restart();
			tries = 1;
		}
	}
	return NULL;
}

/* Texels in the UI heap, such as glyphs rasterised at run time, can be
 * freed and their address reused for others, so those are told apart by
 * a checksum of their contents as well. Of a texture over 4 KiB only
 * every eighth row counts: hashing every texel of the images a repaint
 * draws took longer than painting them. */
static uint32_t checksum(const uint8_t *data, uint32_t stride, uint32_t bytes, uint32_t height)
{
	uint32_t sum = 2166136261u;
	if ((uintptr_t)data < UI_HEAP_BASE || (uintptr_t)data >= UI_HEAP_BASE + UI_HEAP_SIZE)
		return 0;
	uint32_t step = bytes * height > 4096 ? 8 : 1;
	for (uint32_t i = 0; i * step < height + step - 1; i++) {
		/* The last row too. */
		uint32_t r = i * step < height ? i * step : height - 1;
		const uint8_t *p = data + r * stride;
		uint32_t c = 0;
		for (; c + 4 <= bytes; c += 4) {
			uint32_t w;
			__builtin_memcpy(&w, p + c, 4);
			sum = (sum ^ w) * 16777619u;
		}
		for (; c < bytes; c++)
			sum = (sum ^ p[c]) * 16777619u;
	}
	return sum;
}

/* --- the scene */

/* A colour as the coloured program's vertices have it: the target takes
 * the shader's red as its third byte, and the texels' first byte (the
 * layer's red, format 0x0c) comes in as blue. */
static void colour_of(uint32_t c, float f[4])
{
	f[0] = (float)(c >> 16 & 0xff) / 255.0f;
	f[1] = (float)(c >> 8 & 0xff) / 255.0f;
	f[2] = (float)(c & 0xff) / 255.0f;
	f[3] = (float)(c >> 24) / 255.0f;
}

static void put_quad(float *v, const struct quad *q, float tw, float th)
{
	const float k = 2.0f / SIZE;
	float x0 = (float)q->x0 * k - 1.0f, x1 = (float)q->x1 * k - 1.0f;
	float y0 = (float)q->y0 * k - 1.0f, y1 = (float)q->y1 * k - 1.0f;
	float u0 = q->u0 / tw, u1 = q->u1 / tw, v0 = q->v0 / th, v1 = q->v1 / th;
	const float c[6][4] = {
		{ x0, y0, u0, v0 }, { x1, y0, u1, v0 }, { x0, y1, u0, v1 },
		{ x1, y0, u1, v0 }, { x1, y1, u1, v1 }, { x0, y1, u0, v1 },
	};
	for (int i = 0; i < 6; i++)
		for (int j = 0; j < 4; j++)
			v[i * 4 + j] = c[i][j];
}

/* Whether n more atlas quads fit in the scene. */
static bool room(unsigned n)
{
	if (sc->vbytes + n * CQUAD_BYTES > SGX_SCENE_VERTICES_SIZE)
		return false;
	const struct sgx_draw *d = sc->nd ? &sc->draws[sc->nd - 1] : NULL;
	unsigned left = d && d->kind == SGX_DRAW_COLOURED ? DRAW_QUADS - d->count / 6u : 0;
	unsigned more = n > left ? (n - left + DRAW_QUADS - 1) / DRAW_QUADS : 0;
	return sc->nd + more <= SGX_DRAW_MAX_DRAWS;
}

/* A quad onto the last draw while it is of the same kind and texture:
 * atlas quads, whatever their colour, are one coloured draw. The vertex
 * room hands out its bytes in order, so a draw's quads stay together. */
static void emit(const struct quad *q, uint32_t tex, uint16_t tw, uint16_t th, uint32_t colour)
{
	uint8_t kind = tex == atlas->gpu ? SGX_DRAW_COLOURED : SGX_DRAW_TEXTURED;
	uint32_t bytes = kind == SGX_DRAW_COLOURED ? CQUAD_BYTES : QUAD_BYTES;
	struct sgx_draw *d = sc->nd ? &sc->draws[sc->nd - 1] : NULL;
	if (!d || d->kind != kind || d->tex != tex || d->count + 6 > DRAW_QUADS * 6) {
		if (sc->nd == SGX_DRAW_MAX_DRAWS) {
			sc->broken = true;
			return;
		}
		d = &sc->draws[sc->nd++];
		*d = (struct sgx_draw){ .kind = kind, .blend = true, .tex = tex, .width = tw, .height = th, .alpha = 255 };
	}
	uint32_t g;
	float *v = sgx_draw_vertices(bytes, &g);
	if (!v || (d->count && g != d->verts + d->count / 6u * bytes)) {
		sc->broken = true;
		return;
	}
	if (!d->count)
		d->verts = g;
	if (kind == SGX_DRAW_COLOURED) {
		float t[24], c[4];
		put_quad(t, q, tw, th);
		colour_of(colour, c);
		for (int i = 0; i < 6; i++) {
			__builtin_memcpy(v + i * 8, t + i * 4, 4 * sizeof *v);
			__builtin_memcpy(v + i * 8 + 4, c, sizeof c);
		}
	} else
		put_quad(v, q, tw, th);
	d->count += 6;
	sc->vbytes += bytes;
	sc->atlas_used |= tex == atlas->gpu;
	st.quads++;
}

static void scene_start(void)
{
	sc->open = sc->head = true;
	sc->broken = sc->atlas_used = false;
	sc->ncopy = sc->nd = sc->nops = 0;
	sc->vbytes = 0;
	sc->y0 = SIZE;
	sc->y1 = 0;
	sc->rclip = rep.rclip;
	memcpy(sc->clip, rep.clip, sizeof sc->clip);
}

static void keep(const struct op *o)
{
	sc->ops[sc->nops++] = *o;
	if (o->op != OP_CLIP)
		add_rows(&sc->y0, &sc->y1, o->clip[1], o->clip[3]);
}

/* The layer onto itself outside the background's rectangles, band by
 * band down the layer. */
static void self_copy(void)
{
	int ys[2 * MAX_COPY + 2];
	unsigned ny = 0;
	ys[ny++] = 0;
	ys[ny++] = SIZE;
	for (unsigned i = 0; i < sc->ncopy; i++) {
		ys[ny++] = sc->copy[i].y0;
		ys[ny++] = sc->copy[i].y1;
	}
	for (unsigned i = 1; i < ny; i++)
		for (unsigned j = i; j && ys[j - 1] > ys[j]; j--) {
			int t = ys[j];
			ys[j] = ys[j - 1];
			ys[j - 1] = t;
		}
	for (unsigned b = 0; b + 1 < ny; b++) {
		int a = ys[b], z = ys[b + 1], xs[MAX_COPY][2];
		unsigned n = 0;
		if (a == z)
			continue;
		for (unsigned i = 0; i < sc->ncopy; i++) {
			if (sc->copy[i].y0 > a || sc->copy[i].y1 < z)
				continue;
			unsigned j = n++;
			for (; j && xs[j - 1][0] > sc->copy[i].x0; j--) {
				xs[j][0] = xs[j - 1][0];
				xs[j][1] = xs[j - 1][1];
			}
			xs[j][0] = sc->copy[i].x0;
			xs[j][1] = sc->copy[i].x1;
		}
		int x = 0;
		for (unsigned i = 0; i <= n; i++) {
			int x0 = i < n ? xs[i][0] : SIZE;
			if (x0 > x) {
				struct quad q = { x, a, x0, z, (float)x, (float)a, (float)x0, (float)z, 0 };
				emit(&q, rep.layer_gpu, SIZE, SIZE, WHITE);
			}
			if (i < n && xs[i][1] > x)
				x = xs[i][1];
		}
	}
}

/* What the head knew written for the GPU: the flat draw a frame starts
 * with, three vertices on one point, the self-copy, and the background's
 * colours. */
static void close_head(void)
{
	sc->head = false;
	const struct slot *white = lookup(&(struct src){ .kind = K_COLOR, .w = 1, .h = 1, .tint = WHITE });
	sgx_draw_discard();
	uint32_t g;
	float *v = sgx_draw_vertices(3 * 24, &g);
	if (!v) {
		sc->broken = true;
		return;
	}
	for (int i = 0; i < 18; i++)
		v[i] = 0.0f;
	sc->draws[0] = (struct sgx_draw){ .verts = g, .count = 3, .kind = SGX_DRAW_FLAT };
	sc->nd = 1;
	sc->vbytes = FLAT_BYTES;
	self_copy();
	for (unsigned i = 0; i < sc->ncopy; i++) {
		if (!sc->copy[i].color)
			continue;
		if (!white) {
			sc->broken = true;
			return;
		}
		float u = (float)white->x + 0.5f, t = (float)white->y + 0.5f;
		struct quad q = { sc->copy[i].x0, sc->copy[i].y0, sc->copy[i].x1, sc->copy[i].y1, u, t, u, t, 0 };
		emit(&q, atlas->gpu, atlas->w, atlas->h, sc->copy[i].color);
	}
}

static void ensure_scene(void)
{
	if (!sc->open)
		scene_start();
	if (sc->head)
		close_head();
}

/* A scene's calls painted by the CPU instead. */
static void replay(const struct scene *s)
{
	soft_paint_rounded_clip(s->rclip ? s->clip : NULL);
	for (unsigned i = 0; i < s->nops; i++) {
		const struct op *o = &s->ops[i];
		if (o->op == OP_RECT)
			soft_paint_rect(o->f[0], o->f[1], o->f[2], o->f[3], o->clip, o->c0, o->flag);
		else if (o->op == OP_TEXTURE)
			soft_paint_texture(o->data, o->stride, o->width, o->height, o->format, o->dx, o->dy, o->dw, o->dh,
					   o->clip, o->c0, o->flag);
		else if (o->op == OP_ROUNDED)
			soft_paint_rounded(o->f[0], o->f[1], o->f[2], o->f[3], o->radii, o->border, o->clip, o->c0, o->c1);
		else
			soft_paint_rounded_clip(o->flag ? o->f : NULL);
	}
	soft_paint_rounded_clip(rep.rclip ? rep.clip : NULL);
}

/* The open scene painted by the CPU, into the repaint's own rows. */
static void replay_open(void)
{
	replay(sc);
	add_rows(&rep.cpu_y0, &rep.cpu_y1, sc->y0, sc->y1);
}

/* The render of the scene in flight waited for. When the GPU failed it
 * is halted and the CPU paints that scene again, into its own layer. */
static bool settle(void)
{
	if (!fl.busy)
		return true;
	fl.busy = false;
	struct sgx_times t = { 0, 0 };
	uint32_t t0 = time_us();
	bool ok = sgx_draw_finish(fl.ticket, &t);
	st.wait_us += time_us() - t0;
	/* What the GPU wrote is in no cache line: the CPU's own were cleaned. */
	invalidate_rows(fl.layer, fl.s->y0, fl.s->y1);
	if (ok) {
		st.render_us += t.render_us;
		return true;
	}
	st.cpu[CPU_FAILED]++;
	log_printf("paint: %s; the CPU paints until sgx paint gpu\n", sgx_draw_failure());
	sgx_log_state();
	sgx_halt();
	dead = true;
	bool other = fl.layer != rep.layer;
	if (other)
		soft_paint_begin(fl.layer, SIZE, SIZE, SIZE);
	replay(fl.s);
	clean_rows(fl.layer, fl.s->y0, fl.s->y1);
	if (other && rep.layer) {
		soft_paint_begin(rep.layer, SIZE, SIZE, SIZE);
		soft_paint_rounded_clip(rep.rclip ? rep.clip : NULL);
	}
	return false;
}

/* Before the CPU writes the repaint's layer. */
static void cpu_touch(void)
{
	if (fl.busy && fl.layer == rep.layer)
		settle();
}

static void submit(void)
{
	clean_rows(rep.layer, rep.cpu_y0, rep.cpu_y1);
	rep.cpu_y0 = SIZE;
	rep.cpu_y1 = 0;
	if (!settle() || dead) {
		replay_open();
		return;
	}
	if (sc->broken) {
		st.cpu[CPU_REFUSED]++;
		if (!told)
			log_printf("paint: a scene with no room for its draws, painted by the CPU\n");
		told = true;
		replay_open();
		return;
	}
	uint32_t was = sgx_draw_get_target();
	const struct sgx_frame f = { 0, sc->draws, sc->nd };
	struct sgx_times t;
	sgx_draw_rows_format(ROWS_PAINT);
	sgx_draw_target(rep.layer_gpu);
	uint32_t ticket = sgx_draw_kick(&f, &t);
	sgx_draw_rows_format(ROWS_COMPOSE);
	sgx_draw_target(was);
	if (ticket) {
		st.scenes++;
		st.ta_us += t.ta_us;
		fl.busy = true;
		fl.layer = rep.layer;
		fl.ticket = ticket;
		fl.s = sc;
		fl.atlas = atlas;
		sc = sc == &scenes[0] ? &scenes[1] : &scenes[0];
		sc->open = false;
		if (!overlap)
			settle();
		return;
	}
	if (sgx_draw_refused()) {
		st.cpu[CPU_REFUSED]++;
		if (!told)
			log_printf("paint: a scene the GPU does not take, painted by the CPU: %s\n", sgx_draw_failure());
		told = true;
	} else {
		st.cpu[CPU_FAILED]++;
		log_printf("paint: %s; the CPU paints until sgx paint gpu\n", sgx_draw_failure());
		sgx_log_state();
		sgx_halt();
		dead = true;
		rep.gpu = false;
	}
	invalidate_rows(rep.layer, sc->y0, sc->y1);
	replay_open();
}

static void flush_scene(int why)
{
	if (!sc->open)
		return;
	if (sc->head && !sc->ncopy) {
		sc->open = false;
		return;
	}
	if (sc->head)
		close_head();
	sc->open = false;
	st.flush[why]++;
	submit();
}

/* Whether the repaint stays on the GPU, once its background is known. */
static void decide(void)
{
	if (rep.decided)
		return;
	rep.decided = true;
	if (!sc->open || !sc->head || !sc->ncopy)
		return;
	uint32_t area = 0;
	for (unsigned i = 0; i < sc->ncopy; i++)
		area += (uint32_t)((sc->copy[i].x1 - sc->copy[i].x0) * (sc->copy[i].y1 - sc->copy[i].y0));
	if (area >= SMALL_PX)
		return;
	sc->open = false;
	rep.gpu = false;
	st.small++;
	cpu_touch();
	replay_open();
}

/* The call goes to the CPU, after what the GPU has of the repaint. */
static void to_cpu(int why, const int32_t clip[4])
{
	flush_scene(FLUSH_CPU);
	cpu_touch();
	st.cpu[why]++;
	add_rows(&rep.cpu_y0, &rep.cpu_y1, clip[1], clip[3]);
}

/* A call's quads drawn from the texels of ns srcs, or false if they do
 * not fit in the atlas-> */
static bool paint_quads(const struct quad *q, unsigned nq, const struct src *s, unsigned ns, uint32_t colour,
			const struct op *o)
{
	const struct slot *slot[5];
	ensure_scene();
	for (int tries = 0;; tries++) {
		uint32_t epoch = atlas_epoch;
		for (unsigned i = 0; i < ns; i++)
			if (!(slot[i] = lookup(&s[i])))
				return false;
		if (epoch == atlas_epoch)
			break;
		if (tries)
			return false;
	}
	ensure_scene();
	if (sc->nops == MAX_OPS || !room(nq)) {
		flush_scene(FLUSH_ROOM);
		ensure_scene();
	}
	keep(o);
	for (unsigned i = 0; i < nq; i++) {
		struct quad r = q[i];
		const struct slot *t = slot[r.src];
		r.u0 += (float)t->x;
		r.u1 += (float)t->x;
		r.v0 += (float)t->y;
		r.v1 += (float)t->y;
		emit(&r, atlas->gpu, atlas->w, atlas->h, colour);
	}
	return true;
}

static bool setup(uint32_t *layer)
{
	if (!sgx_draw_setup())
		return false;
	if (!atlas->texels || atlas->gen != sgx_generation())
		atlas = &atlases[0];
	for (unsigned i = 0; i < 1u + overlap; i++) {
		struct atlas *a = &atlases[i];
		if (a->texels && a->gen == sgx_generation())
			continue;
		void *cpu;
		a->texels = NULL;
		a->w = atlas_w;
		a->h = atlas_h;
		if (!(a->gpu = sgx_draw_alloc(atlas_w * atlas_h * 4, &cpu))) {
			a->w = SMALL_W;
			a->h = SMALL_H;
			if (!(a->gpu = sgx_draw_alloc(SMALL_W * SMALL_H * 4, &cpu))) {
				if (!i)
					return false;
				break;
			}
		}
		a->texels = cpu;
		a->gen = sgx_generation();
		struct atlas *was = atlas;
		atlas = a;
		reset_atlas();
		atlas = was;
	}
	rep.layer_gpu = sgx_draw_map(PHYS(layer), SIZE * PITCH);
	return rep.layer_gpu != 0;
}

/* --- paint.h */

bool paint_begin(uint32_t *layer, uint32_t width, uint32_t height, uint32_t stride_px)
{
	if (fl.busy && fl.layer == layer)
		settle();
	soft_paint_begin(layer, width, height, stride_px);
	bounds_begin(layer);
	memset(&rep, 0, sizeof rep);
	rep.layer = layer;
	rep.cpu_y0 = SIZE;
	sc->open = false;
	if (mode != PAINT_MODE_GPU)
		return true;
	st.repaints++;
	rep.gpu = !dead && width == SIZE && height == SIZE && stride_px == SIZE && gpu_ready() && setup(layer);
	st.off += !rep.gpu;
	return true;
}

void paint_rounded_clip(const float *clip)
{
	soft_paint_rounded_clip(clip);
	if (!rep.gpu)
		return;
	rep.rclip = clip != NULL;
	if (clip) {
		memcpy(rep.clip, clip, sizeof rep.clip);
		rep.rx0 = round_px(clip[0]);
		rep.ry0 = round_px(clip[1]);
		rep.rx1 = round_px(clip[2]);
		rep.ry1 = round_px(clip[3]);
		float w = (float)(rep.rx1 - rep.rx0), h = (float)(rep.ry1 - rep.ry0), most = (w < h ? w : h) * 0.5f;
		rep.radius = clip[4] < 0.0f ? 0.0f : clip[4] > most ? most : clip[4];
	}
	if (!sc->open)
		return;
	if (sc->nops == MAX_OPS) {
		flush_scene(FLUSH_ROOM);
		return;
	}
	struct op o = { .op = OP_CLIP, .flag = rep.rclip };
	memcpy(o.f, rep.clip, sizeof o.f);
	keep(&o);
}

/* A rectangle of the repaint's background: one of the head's, left out
 * of the self-copy, so a copy of it blends over the clear's 0. */
static void copy_rect(float fx0, float fy0, float fx1, float fy1, const int32_t clip[4], uint32_t color)
{
	if (rep.rclip) {
		decide();
		if (rep.gpu)
			to_cpu(CPU_COPY_CLIP, clip);
		cpu_touch();
		soft_paint_rect(fx0, fy0, fx1, fy1, clip, color, true);
		return;
	}
	int r[4] = { round_px(fx0), round_px(fy0), round_px(fx1), round_px(fy1) };
	if (!cut(r, clip))
		return;
	if (sc->open && (!sc->head || sc->ncopy == MAX_COPY)) {
		decide();
		if (!rep.gpu) {
			cpu_touch();
			soft_paint_rect(fx0, fy0, fx1, fy1, clip, color, true);
			return;
		}
		flush_scene(FLUSH_COPY);
	}
	if (!sc->open)
		scene_start();
	sc->copy[sc->ncopy].x0 = r[0];
	sc->copy[sc->ncopy].y0 = r[1];
	sc->copy[sc->ncopy].x1 = r[2];
	sc->copy[sc->ncopy].y1 = r[3];
	sc->copy[sc->ncopy++].color = color;
	struct op o = { .op = OP_RECT, .flag = true, .c0 = color, .f = { fx0, fy0, fx1, fy1 } };
	memcpy(o.clip, clip, sizeof o.clip);
	keep(&o);
	st.rects++;
}

void paint_rect(float fx0, float fy0, float fx1, float fy1, const int32_t clip[4], uint32_t color, bool copy)
{
	int n[4] = { floor_px(fx0), floor_px(fy0), ceil_px(fx1), ceil_px(fy1) };
	if (cut(n, clip)) {
		if (color)
			note(n);
		/* What a copy covers in whole pixels is replaced. */
		int w[4] = { ceil_px(fx0), ceil_px(fy0), floor_px(fx1), floor_px(fy1) };
		if (copy && !rep.rclip && cut(w, clip))
			note_clear(w);
	}
	if (rep.gpu && copy) {
		copy_rect(fx0, fy0, fx1, fy1, clip, color);
		return;
	}
	if (rep.gpu)
		decide();
	if (!rep.gpu) {
		cpu_touch();
		soft_paint_rect(fx0, fy0, fx1, fy1, clip, color, copy);
		return;
	}
	int r[4] = { round_px(fx0), round_px(fy0), round_px(fx1), round_px(fy1) };
	if (!cut(r, clip) || !color)
		return;
	if (clear_of_corners(r)) {
		const struct src s = { .kind = K_COLOR, .w = 1, .h = 1, .tint = WHITE };
		const struct quad q = { r[0], r[1], r[2], r[3], 0.5f, 0.5f, 0.5f, 0.5f, 0 };
		struct op o = { .op = OP_RECT, .c0 = color, .f = { fx0, fy0, fx1, fy1 } };
		memcpy(o.clip, clip, sizeof o.clip);
		if (paint_quads(&q, 1, &s, 1, color, &o)) {
			st.rects++;
			return;
		}
		to_cpu(CPU_BIG, clip);
	} else
		to_cpu(CPU_CLIP, clip);
	soft_paint_rect(fx0, fy0, fx1, fy1, clip, color, false);
}

/* A texture on the GPU, cut to r: -1, or why the CPU paints it. */
static int gpu_texture(const uint8_t *data, uint32_t stride, uint32_t width, uint32_t height, int format, int dx, int dy,
		       int dw, int dh, const int32_t clip[4], uint32_t tint, bool colorize, const int r[4])
{
	/* The texels go in the atlas as they are, glyphs and alpha maps as
	 * white of their coverage, and the tint is the vertices' colour. */
	struct src s = { .data = data, .stride = stride, .w = width, .h = height, .format = (uint8_t)format, .tint = WHITE,
			 .kind = format == PAINT_ALPHA || colorize ? K_ALPHA : K_IMAGE };
	if (!tint)
		return -1;
	s.sum = checksum(data, stride, width * (format == PAINT_ALPHA ? 1 : format == PAINT_RGB ? 3 : 4), height);
	struct quad q = { dx, dy, dx + dw, dy + dh, 0.0f, 0.0f, (float)width, (float)height, 0 };
	clip_quad(&q, r);
	struct op o = { .op = OP_TEXTURE, .format = (int8_t)format, .flag = colorize, .data = data, .stride = stride,
			.width = width, .height = height, .c0 = tint, .dx = dx, .dy = dy, .dw = dw, .dh = dh };
	memcpy(o.clip, clip, sizeof o.clip);
	if (!paint_quads(&q, 1, &s, 1, tint, &o))
		return CPU_BIG;
	st.textures++;
	return -1;
}

bool paint_texture(const uint8_t *data, uint32_t stride, uint32_t width, uint32_t height, int format, int dx, int dy,
		   int dw, int dh, const int32_t clip[4], uint32_t tint, bool colorize)
{
	if (tint) {
		int n[4] = { dx, dy, dx + dw, dy + dh };
		if (cut(n, clip))
			note(n);
	}
	if (rep.gpu)
		decide();
	if (!rep.gpu) {
		cpu_touch();
		return soft_paint_texture(data, stride, width, height, format, dx, dy, dw, dh, clip, tint, colorize);
	}
	bool scaled = dw != (int)width || dh != (int)height;
	int why = format < PAINT_RGB || format > PAINT_ALPHA || !width || !height || dw <= 0 || dh <= 0 ? CPU_FORMAT :
		  scaled && format == PAINT_ALPHA				     ? CPU_SCALED_ALPHA :
		  width > atlas->w || height > atlas->h				     ? CPU_BIG :
										       -1;
	if (why < 0) {
		int r[4] = { dx, dy, dx + dw, dy + dh };
		if (!cut(r, clip))
			return true;
		why = clear_of_corners(r) ?
			      gpu_texture(data, stride, width, height, format, dx, dy, dw, dh, clip, tint, colorize, r) :
			      CPU_CLIP;
		if (why < 0)
			return true;
	}
	to_cpu(why, clip);
	return soft_paint_texture(data, stride, width, height, format, dx, dy, dw, dh, clip, tint, colorize);
}

/* A filled rounded rectangle as soft.c paints it: each corner a mask of
 * its coverage times the colour, as wide as the radius rounded up and as
 * tall as the rows its arc crosses, and bands of plain colour for the
 * rest. */
bool paint_rounded(float fx0, float fy0, float fx1, float fy1, const float radii[4], float border, const int32_t clip[4],
		   uint32_t fill, uint32_t edge)
{
	if (fill || (edge && border > 0.0f)) {
		int n[4] = { floor_px(fx0), floor_px(fy0), ceil_px(fx1), ceil_px(fy1) };
		if (cut(n, clip))
			note(n);
	}
	if (rep.gpu)
		decide();
	if (!rep.gpu) {
		cpu_touch();
		return soft_paint_rounded(fx0, fy0, fx1, fy1, radii, border, clip, fill, edge);
	}
	if (border > 0.0f) {
		to_cpu(CPU_BORDER, clip);
		return soft_paint_rounded(fx0, fy0, fx1, fy1, radii, border, clip, fill, edge);
	}
	int x0 = round_px(fx0), y0 = round_px(fy0), x1 = round_px(fx1), y1 = round_px(fy1);
	int c[4] = { x0, y0, x1, y1 };
	if (x1 <= x0 || y1 <= y0 || !cut(c, clip) || !fill)
		return true;
	float w = (float)(x1 - x0), h = (float)(y1 - y0), most = (w < h ? w : h) * 0.5f, r[4];
	int cols[4], rows[4];
	struct src s[5] = { { .kind = K_COLOR, .w = 1, .h = 1, .tint = WHITE } };
	uint8_t which[4] = { 0 };
	unsigned ns = 1, nq = 0;
	for (int k = 0; k < 4; k++) {
		r[k] = radii[k] < 0.0f ? 0.0f : radii[k] > most ? most : radii[k];
		cols[k] = ceil_px(r[k]);
		rows[k] = ceil_px(r[k] - 0.5f);
		if (!cols[k] || !rows[k]) {
			cols[k] = rows[k] = 0;
			continue;
		}
		unsigned j = 1;
		while (j < ns && s[j].r != r[k])
			j++;
		if (j == ns)
			s[ns++] = (struct src){ .kind = K_CORNER, .w = (uint32_t)cols[k], .h = (uint32_t)rows[k], .tint = WHITE,
						.r = r[k] };
		which[k] = (uint8_t)j;
	}
	struct quad q[MAX_QUADS];
	/* Corners in the order of the radii, each the top-left mask flipped
	 * into place. */
	for (int k = 0; k < 4; k++) {
		if (!cols[k])
			continue;
		bool right = k == 1 || k == 2, bottom = k >= 2;
		float cw = (float)cols[k], ch = (float)rows[k];
		int qx = right ? x1 - cols[k] : x0, qy = bottom ? y1 - rows[k] : y0;
		q[nq++] = (struct quad){ qx,	qy,	qx + cols[k], qy + rows[k], right ? cw : 0.0f, bottom ? ch : 0.0f,
					 right ? 0.0f : cw, bottom ? 0.0f : ch, which[k] };
	}
	int ys[6] = { y0, y0 + rows[0], y0 + rows[1], y1 - rows[2], y1 - rows[3], y1 };
	for (int i = 1; i < 6; i++)
		for (int j = i; j && ys[j - 1] > ys[j]; j--) {
			int t = ys[j];
			ys[j] = ys[j - 1];
			ys[j - 1] = t;
		}
	for (int i = 0; i < 5; i++) {
		int a = ys[i], b = ys[i + 1];
		if (a >= b)
			continue;
		int left = a < y0 + rows[0] ? cols[0] : a >= y1 - rows[3] ? cols[3] : 0;
		int right = a < y0 + rows[1] ? cols[1] : a >= y1 - rows[2] ? cols[2] : 0;
		if (x0 + left < x1 - right)
			q[nq++] = (struct quad){ x0 + left, a, x1 - right, b, 0.5f, 0.5f, 0.5f, 0.5f, 0 };
	}
	unsigned n = 0;
	for (unsigned i = 0; i < nq; i++) {
		if (!clip_quad(&q[i], c))
			continue;
		if (!clear_of_corners((int[4]){ q[i].x0, q[i].y0, q[i].x1, q[i].y1 })) {
			to_cpu(CPU_CLIP, clip);
			return soft_paint_rounded(fx0, fy0, fx1, fy1, radii, border, clip, fill, edge);
		}
		q[n++] = q[i];
	}
	struct op o = { .op = OP_ROUNDED, .c0 = fill, .c1 = edge, .f = { fx0, fy0, fx1, fy1 }, .border = border };
	memcpy(o.radii, radii, sizeof o.radii);
	memcpy(o.clip, clip, sizeof o.clip);
	if (n && !paint_quads(q, n, s, ns, fill, &o)) {
		to_cpu(CPU_BIG, clip);
		return soft_paint_rounded(fx0, fy0, fx1, fy1, radii, border, clip, fill, edge);
	}
	st.rounded++;
	return true;
}

void paint_cpu(uint32_t y0, uint32_t y1)
{
	note_rows(y0, y1);
	soft_paint_cpu(y0, y1);
	if (rep.gpu)
		add_rows(&rep.cpu_y0, &rep.cpu_y1, (int)y0, (int)y1);
}

void paint_sync(void)
{
	soft_paint_sync();
	if (rep.gpu)
		decide();
	if (rep.gpu)
		flush_scene(FLUSH_SYNC);
	cpu_touch();
}

void paint_end(void)
{
	if (rep.gpu)
		decide();
	if (rep.gpu)
		flush_scene(FLUSH_END);
	if (rep.layer && mode == PAINT_MODE_GPU)
		clean_rows(rep.layer, rep.cpu_y0, rep.cpu_y1);
	rep.gpu = false;
	rep.layer = NULL;
	soft_paint_end();
}

/* With overlap a repaint's last scene renders on until the compositor
 * or the next CPU write to its layer needs it: paint_wait. */
void paint_finish(void)
{
	soft_paint_finish();
	if (!overlap)
		settle();
}

void paint_wait(void) { settle(); }

void paint_overlap(bool on)
{
	settle();
	overlap = on;
}

bool paint_overlap_on(void) { return overlap; }

/* New atlases of w x h from the next repaint on the GPU. The GPU's
 * memory is not given back, so the old ones stay allocated. */
bool paint_atlas(uint32_t w, uint32_t h)
{
	if (w % 32 || !w || !h || w > WIDE_MAX || h > 2048 || !(w & (w - 1)) || !(h & (h - 1)))
		return false;
	settle();
	atlas_w = w;
	atlas_h = h;
	for (unsigned i = 0; i < 2; i++)
		atlases[i].texels = NULL;
	return true;
}

void paint_atlas_size(uint32_t *w, uint32_t *h, uint32_t *now_w, uint32_t *now_h)
{
	*w = atlas_w;
	*h = atlas_h;
	*now_w = atlases[0].texels ? atlases[0].w : 0;
	*now_h = atlases[0].texels ? atlases[0].h : 0;
}

void paint_log(void)
{
	soft_paint_log();
	if (!st.repaints)
		return;
	uint32_t n = st.scenes ? st.scenes : 1;
	log_printf("paint gpu: %lu scenes, %lu repaints (small %lu, off %lu), rect %lu, texture %lu, rounded %lu, quads %lu, "
		   "uploads %lu (px %lu), restarts %lu, switches %lu, geometry %lu us, render %lu us\n",
		   st.scenes, st.repaints, st.small, st.off, st.rects, st.textures, st.rounded, st.quads, st.uploads,
		   st.upload_px, st.restarts, st.switches, st.ta_us / n, st.render_us / n);
	log_printf("paint gpu: wait %lu us%s; flush sync %lu room %lu copy %lu atlas %lu cpu %lu; cpu clip %lu scaled alpha "
		   "%lu border %lu big %lu format %lu copy clip %lu refused %lu failed %lu\n",
		   st.wait_us / n, overlap ? " (overlap)" : "", st.flush[FLUSH_SYNC], st.flush[FLUSH_ROOM], st.flush[FLUSH_COPY], st.flush[FLUSH_ATLAS],
		   st.flush[FLUSH_CPU], st.cpu[CPU_CLIP], st.cpu[CPU_SCALED_ALPHA], st.cpu[CPU_BORDER], st.cpu[CPU_BIG],
		   st.cpu[CPU_FORMAT], st.cpu[CPU_COPY_CLIP], st.cpu[CPU_REFUSED], st.cpu[CPU_FAILED]);
	memset(&st, 0, sizeof st);
}
