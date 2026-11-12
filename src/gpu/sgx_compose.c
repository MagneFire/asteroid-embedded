/* The drawing calls of draw.h on minnow: each frame goes to the SGX530
 * (sgx_draw.c) while it is on and has not failed, otherwise to the CPU
 * (soft.c). On the GPU a frame is a list of draws: the mesh first, flat
 * coloured, then every layer, indicator and later triangle as textured
 * triangles blended over it. The GPU's picture has GL's origin at the
 * bottom, so every position is flipped; turning the picture turns the
 * vertices instead of the finished frame. Layers are read where the UI
 * left them; a texture of another size than the layers' is copied into
 * a slot of a texture of the layers' size, and triangles after the mesh
 * are drawn from a palette in it, a texel per triangle, which gives them
 * blending without a second flat program. The calls of a GPU frame are
 * kept, so that when the GPU fails or refuses a frame the CPU draws the
 * frame from them; after a failure the CPU draws every frame until
 * sgx_compose(true). */
#include <string.h>
#include "asteroid.h"
#include "gpu/a3xx.h"
#include "gpu/soft.h"
#include "gpu/sgx.h"
#include "gpu/sgx_draw.h"
#include "gpu/sgx_compose.h"
#include "display/display.h"

#ifdef HOST_TEST
extern uintptr_t gpu_host_base;
#define PTR(addr)	((void *)(gpu_host_base + (addr)))
#else
#define PTR(addr)	((void *)(uintptr_t)(addr))
#endif

#define SIZE		SGX_DRAW_SIZE
#define PITCH		(SIZE * 4)
#define SLOT		32
#define SLOTS		(SIZE / SLOT)
/* The palette's first row, after the slots. */
#define PALETTE_Y	SLOT
#define MAX_OPS		48
#define MAX_RECTS	128
#define LOG_MS		1000
/* Whether the GPU composites from boot, or only from sgx_compose(true). */
#define AT_BOOT		false

static struct sgx_region regions[3];
static unsigned nregions;
static struct { uint32_t addr, size, gpu; } maps[3];
static unsigned nmaps;
static bool wanted, on_gpu, by_hand = !AT_BOOT, lost;
static uint32_t aux_gpu, *aux, gen;
static char told[8][64];
static unsigned ntold;

static struct sgx_draw draws[SGX_DRAW_MAX_DRAWS];
static unsigned ndraws, slots, palette;
static struct gpu_target target;
static uint32_t target_gpu;
static bool blend;
/* Why the GPU cannot take the frame, or NULL. */
static const char *refusal;
static float turn_c = 1.0f, turn_s;
static struct { uintptr_t from, to; } turned[4];
static unsigned nturned;

/* The frame's calls, for the CPU to draw it from. */
enum { OP_BLEND, OP_TURN, OP_TRIANGLES, OP_PARTS, OP_ROUND };
static struct op {
	uint8_t op, ncomp[2];
	bool on;
	float c, s, alpha;
	uint32_t addr, width, height, pitch, n, stride, nattrs;
	int x, y;
	uint16_t rect;
} ops[MAX_OPS];
static struct gpu_rect rects[MAX_RECTS];
static unsigned nops, nrects;
static bool kept;

static uint32_t stat_t, stat_frames, stat_ta, stat_3d;

static uint32_t gpu_of(uint32_t addr, uint32_t size)
{
	for (unsigned i = 0; i < nmaps; i++)
		if (addr >= maps[i].addr && addr - maps[i].addr + size <= maps[i].size)
			return maps[i].gpu + addr - maps[i].addr;
	return 0;
}

void sgx_compose_regions(const struct sgx_region *r, unsigned n)
{
	nregions = n < ARRAY_SIZE(regions) ? n : ARRAY_SIZE(regions);
	memcpy(regions, r, nregions * sizeof *r);
}

static bool start(void)
{
#ifndef HOST_TEST
	if (!nregions) {
		uint32_t size;
		void *pool = soft_pool(&size);
		const struct sgx_region r[] = { { FB_BASE, FB_SIZE }, { UI_LAYER_BASE, UI_LAYER_SIZE },
						{ (uint32_t)(uintptr_t)pool, size } };
		sgx_compose_regions(r, ARRAY_SIZE(r));
	}
#endif
	if (!sgx_start() || !sgx_draw_setup())
		return false;
	if (gen != sgx_generation()) {
		aux = NULL;
		nmaps = 0;
		gen = sgx_generation();
	}
	if (!aux && !(aux_gpu = sgx_draw_alloc(SIZE * PITCH, (void **)&aux)))
		return false;
	if (!nmaps)
		for (unsigned i = 0; i < nregions; i++) {
			const struct sgx_region *r = &regions[i];
			uint32_t at = r->addr & ~(4096u - 1), end = (r->addr + r->size + 4095) & ~(4096u - 1);
			if (!(maps[nmaps].gpu = sgx_draw_map(at, end - at))) {
				log_printf("sgx: %08lx not mapped\n", r->addr);
				/* Tried again whole next time: what is mapped is found again. */
				nmaps = 0;
				return false;
			}
			maps[nmaps].addr = at;
			maps[nmaps++].size = end - at;
		}
	return true;
}

/* Any failure to start leaves the GPU halted. */
static bool turn_on(void)
{
	if (start())
		return true;
	sgx_halt();
	return false;
}

bool sgx_compose(bool on)
{
	by_hand = !on;
	if (on && lost)
		log_printf("sgx: frames are drawn without the compositor since a lost frame\n");
	wanted = on && !lost && turn_on();
	if (!on)
		sgx_stop();
	return wanted;
}

bool sgx_compose_on(void) { return wanted && !sgx_failed(); }

/* --- the frame on the GPU */

static void begin(const struct gpu_target *t)
{
	target = *t;
	target_gpu = gpu_of(t->base, t->pitch_bytes * t->height);
	refusal = target_gpu && !t->rgba && t->width == SIZE && t->height == SIZE && t->pitch_bytes == PITCH
			  ? NULL
			  : "a target it does not draw into";
	ndraws = slots = palette = nturned = nops = nrects = 0;
	sgx_draw_discard();
	kept = true;
	blend = false;
	turn_c = 1.0f;
	turn_s = 0.0f;
}

static void refuse(const char *why)
{
	if (!refusal)
		refusal = why;
}

static struct sgx_draw *next_draw(void)
{
	if (ndraws == SGX_DRAW_MAX_DRAWS) {
		refuse("more draws than a frame takes");
		return NULL;
	}
	return &draws[ndraws++];
}

/* A position in clip space, y up, as the GPU takes it: turned, flipped. */
static void place(float *out, float x, float y, bool turn)
{
	if (turn) {
		float tx = x * turn_c + y * turn_s;
		y = y * turn_c - x * turn_s;
		x = tx;
	}
	out[0] = x;
	out[1] = -y;
}

static bool is_turned(uintptr_t p)
{
	for (unsigned i = 0; i < nturned; i++)
		if (p >= turned[i].from && p < turned[i].to)
			return true;
	return false;
}

static uint8_t channel(float v)
{
	int i = (int)(v * 255.0f + 0.5f);
	return i < 0 ? 0 : i > 255 ? 255 : (uint8_t)i;
}

static uint8_t fade_of(float alpha)
{
	return channel(alpha);
}

static void textured(uint32_t verts, unsigned count, uint32_t tex, uint8_t alpha)
{
	struct sgx_draw *d = next_draw();
	if (d)
		*d = (struct sgx_draw){ .verts = verts, .count = (uint16_t)count, .kind = SGX_DRAW_TEXTURED, .blend = true,
					.tex = tex, .width = SIZE, .height = SIZE, .alpha = alpha };
}

/* The mesh: the first triangles of the frame, flat coloured. Later ones
 * take their colour from the palette, a texel each. */
static void triangles(uint32_t vbo_addr, unsigned stride, unsigned nattrs, const uint8_t *ncomp, unsigned nverts)
{
	if (nattrs != 2 || ncomp[0] != 4 || ncomp[1] != 4 || stride < 32) {
		refuse("triangles of another vertex layout");
		return;
	}
	const uint8_t *src = PTR(vbo_addr);
	nverts -= nverts % 3;
	bool flat = !ndraws && !blend;
	for (unsigned at = 0; at < nverts && !refusal;) {
		unsigned n = nverts - at > SGX_DRAW_MAX_VERTS ? SGX_DRAW_MAX_VERTS - SGX_DRAW_MAX_VERTS % 3 : nverts - at;
		uint32_t gpu;
		float *v = sgx_draw_vertices(n * (flat ? 24 : 16), &gpu);
		if (!v || (!flat && palette + n / 3 > SIZE * (SIZE - PALETTE_Y))) {
			refuse("no room for the triangles");
			return;
		}
		for (unsigned i = 0; i < n; i++) {
			const float *f = (const float *)(const void *)(src + (at + i) * stride);
			place(v, f[0], f[1], is_turned((uintptr_t)f));
			if (flat) {
				memcpy(&v[2], &f[4], 16);
				v += 6;
				continue;
			}
			unsigned p = palette + i / 3;
			if (i % 3 == 0) {
				const float *c = (const float *)(const void *)(src + (at + i) * stride) + 4;
				uint8_t a = blend ? channel(c[3]) : 255;
				uint8_t t[4] = { channel(c[0]), channel(c[1]), channel(c[2]), a };
				memcpy(&aux[(PALETTE_Y + p / SIZE) * SIZE + p % SIZE], t, 4);
			}
			v[2] = ((float)(p % SIZE) + 0.5f) / SIZE;
			v[3] = ((float)(PALETTE_Y + p / SIZE) + 0.5f) / SIZE;
			v += 4;
		}
		if (flat) {
			struct sgx_draw *d = next_draw();
			if (!d)
				return;
			*d = (struct sgx_draw){ .verts = gpu, .count = (uint16_t)n, .kind = SGX_DRAW_FLAT };
			flat = false;
		} else {
			textured(gpu, n, aux_gpu, 255);
			palette += n / 3;
		}
		at += n;
	}
}

/* The texture a layer call draws from, and where in it the layer is. */
static uint32_t texture(uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch, float *u0, float *v0)
{
	*u0 = *v0 = 0.0f;
	uint32_t gpu = gpu_of(addr, pitch * height);
	if (gpu && !(gpu & 4095) && width == SIZE && height == SIZE && pitch == PITCH)
		return gpu;
	if (width > SLOT || height > SLOT || slots == SLOTS)
		return 0;
	/* Copied, as the GPU's textures are of the layers' width; only one
	 * that size has been recorded. */
	const uint8_t *src = PTR(addr);
	for (uint32_t y = 0; y < height; y++)
		memcpy(&aux[y * SIZE + slots * SLOT], src + y * pitch, width * 4);
	*u0 = (float)(slots++ * SLOT) / SIZE;
	return aux_gpu;
}

static void vertex(float *v, float px, float py, float u, float t)
{
	place(v, 2.0f * px / SIZE - 1.0f, 1.0f - 2.0f * py / SIZE, true);
	v[2] = u;
	v[3] = t;
}

static void parts(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch, int x, int y, float alpha,
		  const struct gpu_rect *r, unsigned n)
{
	uint8_t fade = fade_of(alpha);
	if (!fade || !n)
		return;
	float u0, v0, su = (float)width / SIZE, sv = (float)height / SIZE;
	uint32_t tex = texture(tex_addr, width, height, pitch, &u0, &v0), gpu;
	float *v = tex && n * 6 <= SGX_DRAW_MAX_VERTS ? sgx_draw_vertices(n * 6 * 16, &gpu) : NULL;
	if (!v) {
		refuse(tex ? "no room for a layer's rectangles" : "a texture it cannot place");
		return;
	}
	for (unsigned i = 0; i < n; i++, v += 24) {
		float x0 = (float)(x + r[i].x), y0 = (float)(y + r[i].y);
		float x1 = x0 + (float)r[i].width, y1 = y0 + (float)r[i].height;
		float a0 = u0 + su * (float)r[i].x / width, b0 = v0 + sv * (float)r[i].y / height;
		float a1 = a0 + su * (float)r[i].width / width, b1 = b0 + sv * (float)r[i].height / height;
		vertex(v, x0, y0, a0, b0);
		vertex(v + 4, x1, y0, a1, b0);
		vertex(v + 8, x0, y1, a0, b1);
		vertex(v + 12, x1, y0, a1, b0);
		vertex(v + 16, x1, y1, a1, b1);
		vertex(v + 20, x0, y1, a0, b1);
	}
	textured(gpu, n * 6, tex, fade);
}

static void round_layer(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch, int x, int y, float alpha)
{
	uint8_t fade = fade_of(alpha);
	if (!fade)
		return;
	float u0, v0, su = (float)width / SIZE, sv = (float)height / SIZE;
	uint32_t tex = texture(tex_addr, width, height, pitch, &u0, &v0), gpu;
	float *v = tex ? sgx_draw_vertices(FLATMESH_ROUND_SIDES * 3 * 16, &gpu) : NULL;
	if (!v) {
		refuse(tex ? "no room for a round layer" : "a texture it cannot place");
		return;
	}
	const float (*edge)[2] = flatmesh_round_edge();
	for (unsigned k = 0; k < FLATMESH_ROUND_SIDES; k++, v += 12) {
		const float *a = edge[k], *b = edge[(k + 1) % FLATMESH_ROUND_SIDES];
		const float p[3][2] = { { 0.5f, 0.5f }, { 0.5f + a[0], 0.5f + a[1] }, { 0.5f + b[0], 0.5f + b[1] } };
		for (unsigned j = 0; j < 3; j++)
			vertex(v + 4 * j, (float)x + p[j][0] * width, (float)y + p[j][1] * height, u0 + su * p[j][0],
			       v0 + sv * p[j][1]);
	}
	textured(gpu, FLATMESH_ROUND_SIDES * 3, tex, fade);
}

/* Once for each kind of frame. */
static void tell(const char *why)
{
	for (unsigned i = 0; i < ntold; i++)
		if (!strcmp(told[i], why))
			return;
	if (ntold == ARRAY_SIZE(told))
		return;
	snprintf(told[ntold++], sizeof told[0], "%s", why);
	log_printf("sgx: a frame the GPU does not take, drawn by the CPU: %s\n", why);
}

static bool end(void)
{
	if (!refusal && (!ndraws || draws[0].kind != SGX_DRAW_FLAT))
		refusal = "no mesh first";
	if (refusal) {
		tell(refusal);
		return false;
	}
	struct sgx_frame f = { 0xff000000u, draws, ndraws };
	struct sgx_times t;
	sgx_draw_target(target_gpu);
	if (!sgx_draw_frame(&f, &t)) {
		if (sgx_draw_refused()) {
			tell(sgx_draw_failure());
			return false;
		}
		log_printf("sgx: %s; the CPU draws until sgx ui on\n", sgx_draw_failure());
		wanted = false;
		sgx_halt();
		return false;
	}
	stat_frames++;
	stat_ta += t.ta_us;
	stat_3d += t.render_us;
	if (time_ms() - stat_t >= LOG_MS) {
		log_printf("sgx: %lu frames, geometry %lu us, render %lu us\n", stat_frames, stat_ta / stat_frames,
			   stat_3d / stat_frames);
		stat_t = time_ms();
		stat_frames = stat_ta = stat_3d = 0;
	}
	return true;
}

/* --- the calls kept for the CPU */

static struct op *keep(uint8_t what)
{
	if (nops == MAX_OPS) {
		kept = false;
		return NULL;
	}
	ops[nops] = (struct op){ .op = what };
	return &ops[nops++];
}

static void keep_layer(uint8_t what, uint32_t tex, uint32_t width, uint32_t height, uint32_t pitch, int x, int y, float alpha,
		       const struct gpu_rect *r, unsigned n)
{
	struct op *o = keep(what);
	if (!o || nrects + n > MAX_RECTS) {
		kept = false;
		return;
	}
	o->addr = tex;
	o->width = width;
	o->height = height;
	o->pitch = pitch;
	o->x = x;
	o->y = y;
	o->alpha = alpha;
	o->rect = (uint16_t)nrects;
	o->n = n;
	memcpy(&rects[nrects], r, n * sizeof *r);
	nrects += n;
}

/* The frame drawn again, by the CPU. */
static bool replay(void)
{
	soft_draw_begin(&target);
	for (unsigned i = 0; i < nops; i++) {
		const struct op *o = &ops[i];
		if (o->op == OP_BLEND)
			soft_draw_blend(o->on);
		else if (o->op == OP_TURN)
			soft_draw_turn(o->c, o->s);
		else if (o->op == OP_TRIANGLES)
			soft_draw_triangles(o->addr, o->stride, o->nattrs, o->ncomp, o->n);
		else if (o->op == OP_PARTS)
			soft_draw_layer_parts(o->addr, o->width, o->height, o->pitch, o->x, o->y, o->alpha, &rects[o->rect], o->n);
		else
			soft_draw_layer_round(o->addr, o->width, o->height, o->pitch, o->x, o->y, o->alpha);
	}
	return soft_draw_end();
}

/* --- draw.h and a3xx.h */

/* At start and after sleep: on the GPU if it was turned on and has not
 * failed since. */
bool gpu_init(void)
{
	wanted = !by_hand && !lost && !sgx_failed() && turn_on();
	return soft_init();
}

bool gpu_selftest(void) { return soft_selftest(); }

void gpu_suspend(void)
{
	sgx_stop();
	soft_suspend();
}

bool gpu_ready(void) { return wanted && sgx_running(); }

void *gpu_alloc(uint32_t size, uint32_t align) { return soft_alloc(size, align); }

void gpu_draw_begin(const struct gpu_target *t)
{
	on_gpu = wanted && sgx_running();
	if (on_gpu)
		begin(t);
	else
		soft_draw_begin(t);
}

bool gpu_draw_program(struct gpu_shader *vs, struct gpu_shader *fs) { return true; }
void gpu_draw_consts(int stage, unsigned first_vec4, const float *v, unsigned nvec4) {}
void gpu_draw_texture(int unit, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch_bytes) {}
void gpu_draw_texture_format(int unit, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch_bytes,
			     enum gpu_tex format)
{
}
void gpu_draw_invalidate_textures(void) {}

void gpu_draw_blend(bool premultiplied_over)
{
	if (!on_gpu) {
		soft_draw_blend(premultiplied_over);
		return;
	}
	blend = premultiplied_over;
	struct op *o = keep(OP_BLEND);
	if (o)
		o->on = premultiplied_over;
}

void gpu_draw_turn(float c, float s)
{
	if (!on_gpu) {
		soft_draw_turn(c, s);
		return;
	}
	turn_c = c;
	turn_s = s;
	struct op *o = keep(OP_TURN);
	if (o) {
		o->c = c;
		o->s = s;
	}
}

/* The vertices are turned as they are given to the GPU, and stay as
 * they are for the CPU, which turns the finished picture instead. */
void gpu_draw_turn_vertices(float *v, unsigned n, unsigned stride)
{
	if (!on_gpu)
		return;
	if (nturned == ARRAY_SIZE(turned)) {
		refuse("more turned vertices than kept");
		return;
	}
	turned[nturned].from = (uintptr_t)v;
	turned[nturned++].to = (uintptr_t)(v + n * stride);
}

void gpu_draw_triangles(uint32_t vbo_addr, unsigned stride, unsigned nattrs, const uint8_t *attr_ncomp, unsigned nverts)
{
	if (!on_gpu) {
		soft_draw_triangles(vbo_addr, stride, nattrs, attr_ncomp, nverts);
		return;
	}
	struct op *o = keep(OP_TRIANGLES);
	if (o) {
		o->addr = vbo_addr;
		o->stride = stride;
		o->nattrs = nattrs;
		o->n = nverts;
		memcpy(o->ncomp, attr_ncomp, nattrs < 2 ? nattrs : 2);
	}
	if (!refusal)
		triangles(vbo_addr, stride, nattrs, attr_ncomp, nverts);
}

void gpu_draw_layer_parts(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha,
			  const struct gpu_rect *r, unsigned n)
{
	if (!on_gpu) {
		soft_draw_layer_parts(tex_addr, width, height, pitch_bytes, x, y, alpha, r, n);
		return;
	}
	keep_layer(OP_PARTS, tex_addr, width, height, pitch_bytes, x, y, alpha, r, n);
	if (!refusal)
		parts(tex_addr, width, height, pitch_bytes, x, y, alpha, r, n);
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

void gpu_draw_layer_round(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha)
{
	if (!on_gpu) {
		soft_draw_layer_round(tex_addr, width, height, pitch_bytes, x, y, alpha);
		return;
	}
	keep_layer(OP_ROUND, tex_addr, width, height, pitch_bytes, x, y, alpha, NULL, 0);
	if (!refusal)
		round_layer(tex_addr, width, height, pitch_bytes, x, y, alpha);
}

/* Before the GPU draws into a buffer the CPU's dirty lines of it are
 * written back, and afterwards nothing stale of it stays cached; written
 * back again, as the CPU may have drawn the frame itself instead. */
void gpu_draw_cpu_sync(void *buf, uint32_t bytes)
{
	if (wanted)
		dcache_clean_invalidate_range(buf, bytes);
}

void gpu_draw_submit(void) {}
bool gpu_draw_wait(void) { return true; }

bool gpu_draw_end(void)
{
	if (!on_gpu)
		return soft_draw_end();
	on_gpu = false;
	if (end() || (kept && replay()))
		return true;
	/* What the CPU cannot draw again is lost, and frame.c draws without
	 * the compositor from now on. */
	log_printf("sgx: a frame lost; the GPU stays off\n");
	lost = true;
	wanted = false;
	sgx_stop();
	return false;
}
