/* The UI's painting on the GPU (sgx_paint.c) on the host, against a fake
 * sgx_draw.c that renders the frames as the SGX530 does with the rows
 * format 0x0c: a clear, then each triangle's pixel centres, textures
 * sampled nearest with their bytes as they are, blended premultiplied;
 * and against a cache: the layer is what the CPU sees, the GPU reads and
 * writes memory of its own, which only a clean fills and only an
 * invalidate shows the CPU. Every repaint must leave both the same as
 * soft.c paints it alone, the frames must keep sgx_draw.h's rules, and
 * the CPU must have no uncleaned line of the layer while a frame runs
 * nor a stale one when the repaint is done, which the comparison with
 * soft.c's pixels shows. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/a3xx.h"
#include "gpu/paint.h"
#include "gpu/soft.h"
#include "gpu/sgx.h"
#include "gpu/sgx_draw.h"
#include "gpu/sgx_scene_data.h"

#define SIZE		SGX_DRAW_SIZE
#define PITCH		(SIZE * 4)
#define ATLAS_GPU	0x10000000u
#define ATLAS2_GPU	0x18000000u
#define LAYER_GPU	0x20000000u
#define VERT_GPU	0x30000000u
#define ATLAS_BYTES	(480 * 1440 * 4)

static struct {
	uint32_t layer[SIZE * SIZE] __attribute__((aligned(4096)));
	uint32_t ref[SIZE * SIZE] __attribute__((aligned(4096)));
} mem __attribute__((aligned(4096)));
uintptr_t gpu_host_base;

static uint32_t dram[SIZE * SIZE], tile[SIZE * SIZE], snap[SIZE * SIZE];
static uint8_t atlas_mem[ATLAS_BYTES] __attribute__((aligned(4096))), atlas2_mem[ATLAS_BYTES] __attribute__((aligned(4096)));
static float vmem[SGX_SCENE_VERTICES_SIZE / 4];

static int failures;
#define CHECK(cond, ...) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
			failures++; \
		} \
	} while (0)

/* --- the firmware around it */

static char logs[4096];
void rotation_buffer(uint32_t *buf, uint32_t stride) {}
uint32_t time_ms(void) { return 0; }
uint32_t time_us(void) { return 0; }
void log_printf(const char *fmt, ...)
{
	size_t n = strlen(logs);
	va_list ap;
	va_start(ap, fmt);
	if (n < sizeof logs - 1)
		vsnprintf(logs + n, sizeof logs - n, fmt, ap);
	va_end(ap);
}

/* The cache, a row at a time: a clean writes what the CPU sees of the
 * layer to memory, an invalidate shows it memory. */
static int layer_row(const void *addr, uint32_t size, int *rows)
{
	const uint8_t *p = addr, *base = (const uint8_t *)mem.layer;
	if (p < base || p >= base + sizeof mem.layer)
		return -1;
	CHECK((p - base) % PITCH == 0 && size % PITCH == 0, "cache maintenance of part of a row");
	*rows = (int)(size / PITCH);
	return (int)((p - base) / PITCH);
}

static unsigned cleans, invalidates;
void dcache_clean_range(const void *addr, uint32_t size)
{
	int n, y = layer_row(addr, size, &n);
	if (y >= 0)
		memcpy(&dram[y * SIZE], &mem.layer[y * SIZE], (size_t)n * PITCH);
	cleans++;
}
void dcache_invalidate_range(const void *addr, uint32_t size)
{
	int n, y = layer_row(addr, size, &n);
	if (y >= 0)
		memcpy(&mem.layer[y * SIZE], &dram[y * SIZE], (size_t)n * PITCH);
	invalidates++;
}
void dcache_clean_invalidate_range(const void *addr, uint32_t size) { dcache_clean_range(addr, size); }

/* --- the GPU, faked */

static bool running = true, failed, frame_ok = true, refuse, refused;
static uint32_t generation = 1, target, vused, layer_phys;
static uint8_t rows_format = 0x0d;
static unsigned frames, halts, max_draws;
static unsigned atlases_given;

bool gpu_ready(void) { return running; }
void sgx_halt(void)
{
	halts++;
	running = false;
	failed = true;
}
bool sgx_failed(void) { return failed; }
uint32_t sgx_generation(void) { return generation; }
void sgx_log_state(void) {}
bool sgx_draw_setup(void) { return running; }

uint32_t sgx_draw_map(uint32_t phys, uint32_t size)
{
	if ((phys | size) & 4095)
		return 0;
	layer_phys = phys;
	return LAYER_GPU;
}

uint32_t sgx_draw_alloc(uint32_t size, void **cpu)
{
	if (atlases_given == 2 || size > sizeof atlas_mem)
		return 0;
	uint8_t *a = atlases_given++ ? atlas2_mem : atlas_mem;
	memset(a, 0, sizeof atlas_mem);
	*cpu = a;
	return a == atlas_mem ? ATLAS_GPU : ATLAS2_GPU;
}

void *sgx_draw_vertices(uint32_t bytes, uint32_t *gpu)
{
	bytes = (bytes + 15) & ~15u;
	if (vused + bytes > sizeof vmem)
		return NULL;
	*gpu = VERT_GPU + vused;
	void *p = (uint8_t *)vmem + vused;
	vused += bytes;
	return p;
}

void sgx_draw_discard(void) { vused = 0; }
void sgx_draw_target(uint32_t gpu_addr) { target = gpu_addr; }
uint32_t sgx_draw_get_target(void) { return target; }
void sgx_draw_rows_format(uint8_t format) { rows_format = format; }
const char *sgx_draw_failure(void) { return refused ? "fake refusal" : "fake failure"; }
bool sgx_draw_refused(void) { return refused; }

static bool power_of_two(uint32_t v) { return !(v & (v - 1)); }

static const uint8_t *texels(uint32_t gpu)
{
	if (gpu == ATLAS_GPU)
		return atlas_mem;
	if (gpu == ATLAS2_GPU)
		return atlas2_mem;
	if (gpu == LAYER_GPU)
		return (const uint8_t *)snap;
	return NULL;
}

static const char *invalid(const struct sgx_frame *f)
{
	static char why[96];
	if (!running)
		return "a frame while the GPU is off";
	if (f->clear || rows_format != 0x0c || target != LAYER_GPU || !f->n || f->n > SGX_DRAW_MAX_DRAWS)
		return "a frame of another clear, format, target or size";
	if (layer_phys != (uint32_t)((uintptr_t)mem.layer - gpu_host_base))
		return "another layer mapped";
	for (unsigned i = 0; i < f->n; i++) {
		const struct sgx_draw *d = &f->draws[i];
		bool flat = d->kind == SGX_DRAW_FLAT;
		uint32_t bytes = d->count * (flat ? 24u : d->kind == SGX_DRAW_COLOURED ? 32u : 16u);
		snprintf(why, sizeof why, "draw %u of %u", i, f->n);
		if (!d->count || d->count % 3 || d->count > SGX_DRAW_MAX_VERTS || d->verts < VERT_GPU ||
		    d->verts - VERT_GPU + bytes > vused || flat != !i || (flat && d->blend))
			return why;
		if (!flat && (!d->blend || d->linear || d->tiled || d->width % 32 || power_of_two(d->width) ||
			      power_of_two(d->height) || d->width > 2048 || d->height > 2048 || !texels(d->tex)))
			return why;
	}
	return NULL;
}

static uint32_t scale8(uint32_t v, uint32_t by)
{
	uint32_t t = v * by + 128;
	return (t + (t >> 8)) >> 8;
}

static double edge(const double *a, const double *b, double x, double y)
{
	return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
}

/* Whether a pixel centre on the edge a-b belongs to the triangle: of two
 * triangles sharing the edge, which run it opposite ways, exactly one. */
static bool owns(const double *a, const double *b)
{
	double dx = b[0] - a[0], dy = b[1] - a[1];
	return dy > 0 || (dy == 0 && dx < 0);
}

static unsigned flat_pixels;

static void triangle(const float *v0, const float *v1, const float *v2, unsigned floats, const struct sgx_draw *d)
{
	const float *src[3] = { v0, v1, v2 };
	double p[3][2];
	for (int i = 0; i < 3; i++) {
		p[i][0] = ((double)src[i][0] + 1.0) * SIZE / 2;
		p[i][1] = ((double)src[i][1] + 1.0) * SIZE / 2;
	}
	double area = edge(p[0], p[1], p[2][0], p[2][1]);
	if (area == 0)
		return;
	int o[3] = { 0, 1, 2 };
	if (area < 0) {
		o[1] = 2;
		o[2] = 1;
		area = -area;
	}
	const double *a = p[o[0]], *b = p[o[1]], *c = p[o[2]];
	const uint8_t *tex = d->kind != SGX_DRAW_FLAT ? texels(d->tex) : NULL;
	int lo[2], hi[2];
	for (int i = 0; i < 2; i++) {
		double mn = p[0][i], mx = p[0][i];
		for (int j = 1; j < 3; j++) {
			mn = p[j][i] < mn ? p[j][i] : mn;
			mx = p[j][i] > mx ? p[j][i] : mx;
		}
		lo[i] = mn < 1 ? 0 : (int)mn - 1;
		hi[i] = mx >= SIZE - 1 ? SIZE - 1 : (int)mx + 1;
	}
	for (int y = lo[1]; y <= hi[1]; y++)
		for (int x = lo[0]; x <= hi[0]; x++) {
			double px = x + 0.5, py = y + 0.5;
			double e0 = edge(b, c, px, py), e1 = edge(c, a, px, py), e2 = edge(a, b, px, py);
			if (e0 < 0 || e1 < 0 || e2 < 0 || (e0 == 0 && !owns(b, c)) || (e1 == 0 && !owns(c, a)) ||
			    (e2 == 0 && !owns(a, b)))
				continue;
			double w[3];
			w[o[0]] = e0 / area;
			w[o[1]] = e1 / area;
			w[o[2]] = e2 / area;
			uint8_t s[4], *t = (uint8_t *)&tile[y * SIZE + x];
			if (!tex) {
				flat_pixels++;
				for (int k = 0; k < 4; k++)
					s[k] = (uint8_t)(src[0][2 + k] * 255.0f + 0.5f);
				uint8_t r = s[0];
				s[0] = s[2];
				s[2] = r;
			} else {
				double u = 0, t2 = 0;
				for (int k = 0; k < 3; k++) {
					u += w[k] * src[k][2];
					t2 += w[k] * src[k][3];
				}
				int tx = (int)(u * d->width), ty = (int)(t2 * d->height);
				tx = tx < 0 ? 0 : tx >= d->width ? d->width - 1 : tx;
				ty = ty < 0 ? 0 : ty >= d->height ? d->height - 1 : ty;
				memcpy(s, tex + ((uint32_t)ty * d->width + (uint32_t)tx) * 4, 4);
				if (d->kind == SGX_DRAW_COLOURED) {
					/* The shader's red lands in the third byte, so the
					 * first texel byte meets blue. */
					static const int ch[4] = { 2, 1, 0, 3 };
					for (int k = 0; k < 4; k++) {
						double c = 0;
						for (int j = 0; j < 3; j++)
							c += w[j] * src[j][4 + ch[k]];
						s[k] = (uint8_t)scale8(s[k], (uint32_t)(c * 255.0 + 0.5));
					}
				} else if (d->alpha != 255)
					for (int k = 0; k < 4; k++)
						s[k] = (uint8_t)scale8(s[k], d->alpha);
			}
			uint32_t keep = 255u - s[3];
			for (int k = 0; k < 4; k++) {
				uint32_t v = s[k] + scale8(t[k], keep);
				t[k] = (uint8_t)(v > 255 ? 255 : v);
			}
			(void)floats;
		}
}

/* The scene as the GPU has it once kicked: its draws and their vertices
 * (the geometry is done), and what rendering it from the memory of then
 * gives, which its render at sgx_draw_finish must match: nothing it
 * samples or the layer may change in between. */
static struct {
	bool busy;
	uint32_t ticket;
	unsigned n;
	struct sgx_draw draws[SGX_DRAW_MAX_DRAWS];
	uint32_t at_kick[SIZE * SIZE], layer[SIZE * SIZE];
} kicked;
static float kicked_v[SGX_SCENE_VERTICES_SIZE / 4];
static uint32_t tickets;
static bool finish_ok = true;
static unsigned finishes;

static void render(void)
{
	memcpy(snap, dram, sizeof snap);
	memset(tile, 0, sizeof tile);
	for (unsigned i = 0; i < kicked.n; i++) {
		const struct sgx_draw *d = &kicked.draws[i];
		unsigned floats = d->kind == SGX_DRAW_FLAT ? 6 : d->kind == SGX_DRAW_COLOURED ? 8 : 4;
		const float *v = kicked_v + (d->verts - VERT_GPU) / 4;
		for (unsigned k = 0; k + 2 < d->count; k += 3)
			triangle(v + k * floats, v + (k + 1) * floats, v + (k + 2) * floats, floats, d);
	}
}

uint32_t sgx_draw_kick(const struct sgx_frame *f, struct sgx_times *t)
{
	CHECK(!kicked.busy, "a frame kicked while another renders");
	frames++;
	const char *why = invalid(f);
	CHECK(!why, "%s", why);
	/* No line the CPU wrote may be left uncleaned while the GPU works. */
	CHECK(!memcmp(dram, mem.layer, sizeof dram), "the layer has uncleaned lines when a frame starts");
	if (f->n > max_draws)
		max_draws = f->n;
	t->ta_us = t->render_us = 100;
	refused = refuse;
	if (why || refuse || !frame_ok) {
		vused = 0;
		return 0;
	}
	kicked.busy = true;
	kicked.ticket = ++tickets;
	kicked.n = f->n;
	memcpy(kicked.draws, f->draws, f->n * sizeof *f->draws);
	memcpy(kicked_v, vmem, vused);
	vused = 0;
	render();
	memcpy(kicked.at_kick, tile, sizeof tile);
	memcpy(kicked.layer, mem.layer, sizeof mem.layer);
	return kicked.ticket;
}

bool sgx_draw_finish(uint32_t ticket, struct sgx_times *t)
{
	if (!kicked.busy || ticket != kicked.ticket)
		return ticket && ticket <= tickets;
	kicked.busy = false;
	finishes++;
	t->render_us = 100;
	CHECK(!memcmp(kicked.layer, mem.layer, sizeof mem.layer), "the CPU wrote a layer the GPU was painting");
	if (!finish_ok)
		return false;
	render();
	CHECK(!memcmp(kicked.at_kick, tile, sizeof tile), "what a rendering scene samples changed under it");
	memcpy(dram, tile, sizeof dram);
	return true;
}

bool sgx_draw_busy(void) { return kicked.busy; }

bool sgx_draw_frame(const struct sgx_frame *f, struct sgx_times *t)
{
	uint32_t ticket = sgx_draw_kick(f, t);
	return ticket && sgx_draw_finish(ticket, t);
}

/* --- what the calls are given */

static uint32_t seed = 1234;
static uint32_t rnd(void)
{
	seed = seed * 1664525u + 1013904223u;
	return seed >> 8;
}

#define GW 13
#define GH 17
static uint8_t glyph[4][GH * GW];
static uint8_t img_pm[30 * 40 * 4], img_st[24 * 32 * 4], img_rgb[20 * 63];
static uint8_t big[100 * 160 * 4];

static uint32_t premul(uint32_t c)
{
	uint32_t a = c >> 24;
	return scale8(c & 0xff, a) | scale8(c >> 8 & 0xff, a) << 8 | scale8(c >> 16 & 0xff, a) << 16 | a << 24;
}

static void fill_inputs(void)
{
	for (unsigned g = 0; g < 4; g++)
		for (unsigned i = 0; i < GH * GW; i++)
			glyph[g][i] = (uint8_t)(rnd() % 3 == 0 ? 0 : rnd() % 3 == 0 ? 255 : rnd());
	for (unsigned i = 0; i < sizeof img_pm; i += 4) {
		uint32_t c = premul(rnd() | (rnd() % 4 == 0 ? 0 : 0x80000000u));
		memcpy(&img_pm[i], &c, 4);
	}
	for (unsigned i = 0; i < sizeof img_st; i++)
		img_st[i] = (uint8_t)rnd();
	for (unsigned i = 0; i < sizeof img_rgb; i++)
		img_rgb[i] = (uint8_t)rnd();
	for (unsigned i = 0; i < sizeof big; i += 4) {
		uint32_t c = premul(rnd());
		memcpy(&big[i], &c, 4);
	}
}

struct api {
	bool (*begin)(uint32_t *, uint32_t, uint32_t, uint32_t);
	void (*rclip)(const float *);
	void (*rect)(float, float, float, float, const int32_t *, uint32_t, bool);
	bool (*texture)(const uint8_t *, uint32_t, uint32_t, uint32_t, int, int, int, int, int, const int32_t *, uint32_t,
			bool);
	bool (*rounded)(float, float, float, float, const float *, float, const int32_t *, uint32_t, uint32_t);
	void (*cpu)(uint32_t, uint32_t);
	void (*sync)(void);
	void (*end)(void);
};

static const struct api gpu_api = { paint_begin, paint_rounded_clip, paint_rect, paint_texture,
				    paint_rounded, paint_cpu,		 paint_sync, paint_end };
static const struct api soft_api = { soft_paint_begin,   soft_paint_rounded_clip, soft_paint_rect, soft_paint_texture,
				     soft_paint_rounded, soft_paint_cpu,	  soft_paint_sync, soft_paint_end };

static const int32_t all[4] = { 0, 0, SIZE, SIZE };
static bool returned[64];
static unsigned nreturned;

static void note(bool r)
{
	if (nreturned < 64)
		returned[nreturned++] = r;
}

static void glyphs(const struct api *a, int x, int y, int n, const int32_t *clip)
{
	static const uint32_t tints[] = { 0xffffffffu, 0xff2040c0u, 0x80402010u, 0xff00ff00u };
	for (int i = 0; i < n; i++)
		note(a->texture(glyph[i % 4], GW, GW, GH, PAINT_ALPHA, x + (i % 16) * (GW + 2), y + (i / 16) * (GH + 2), GW,
				GH, clip, tints[i % 4], false));
}

/* Everything the GPU paints, over an opaque background. */
static void full(const struct api *a, uint32_t *buf)
{
	const float r10[4] = { 10, 10, 10, 10 }, mixed[4] = { 0, 6.5f, 20, 3 }, pill[4] = { 7.5f, 7.5f, 7.5f, 7.5f };
	const int32_t left[4] = { 0, 0, 180, SIZE }, top[4] = { 0, 0, SIZE, 128 };
	a->begin(buf, SIZE, SIZE, SIZE);
	a->rect(0, 0, SIZE, SIZE, all, 0xff402010u, true);
	a->rect(10, 10, 100, 60, all, 0xffffffffu, false);
	a->rect(50.4f, 40.6f, 150, 90, all, 0x80402000u, false);
	glyphs(a, 20, 100, 24, top);
	note(a->texture(img_pm, 160, 40, 30, PAINT_RGBA_PREMULTIPLIED, 200, 10, 40, 30, all, 0xffffffffu, false));
	note(a->texture(img_st, 128, 32, 24, PAINT_RGBA, 250, 50, 32, 24, all, 0x80808080u, false));
	note(a->texture(img_rgb, 63, 21, 20, PAINT_RGB, 10, 200, 21, 20, all, 0xffffffffu, false));
	note(a->texture(img_pm, 160, 40, 30, PAINT_RGBA_PREMULTIPLIED, 40, 240, 80, 60, all, 0xffffffffu, false));
	note(a->texture(img_st, 128, 32, 24, PAINT_RGBA, 150, 250, 32, 24, all, 0xff2080c0u, true));
	note(a->texture(img_pm, 160, 40, 30, PAINT_RGBA_PREMULTIPLIED, 290, 300, 40, 30, all, 0x40404040u, false));
	note(a->rounded(120, 180, 220, 230, r10, 0, left, 0xff00a0ffu, 0xff00a0ffu));
	note(a->rounded(130, 260, 300, 315, mixed, 0, all, 0xc0604020u, 0xc0604020u));
	note(a->rounded(20, 290, 100, 305, pill, 0, all, 0xffc0c0c0u, 0xffc0c0c0u));
	a->end();
}

/* A part of a layer already painted: two rectangles of background, one
 * clear, and what goes in them, clipped to them as Slint clips. */
static void partial(const struct api *a, uint32_t *buf)
{
	const int32_t r0[4] = { 40, 50, 200, 180 }, r1[4] = { 220, 10, 300, 60 };
	const float r6[4] = { 6, 6, 6, 6 };
	a->begin(buf, SIZE, SIZE, SIZE);
	a->rect(40, 50, 200, 180, r0, 0, true);
	a->rect(220, 10, 300, 60, r1, 0xff112233u, true);
	for (int k = 0; k < 2; k++) {
		const int32_t *c = k ? r1 : r0;
		a->rect(30, 40, 260, 70, c, 0xc0808080u, false);
		glyphs(a, 30, 45, 20, c);
		note(a->rounded(180, 30, 240, 120, r6, 0, c, 0xff8000ffu, 0xff8000ffu));
		note(a->texture(img_pm, 160, 40, 30, PAINT_RGBA_PREMULTIPLIED, 190, 150, 40, 30, c, 0xffffffffu, false));
	}
	a->end();
}

/* A rounded clip: what keeps clear of its corners on the GPU, the rest on
 * the CPU. */
static void rounded_clip(const struct api *a, uint32_t *buf)
{
	const float clip[5] = { 20, 20, 300, 300, 30 }, r4[4] = { 4, 4, 4, 4 };
	a->begin(buf, SIZE, SIZE, SIZE);
	a->rect(0, 0, SIZE, SIZE, all, 0xff000000u, true);
	a->rclip(clip);
	a->rect(100, 100, 200, 200, all, 0xffff0000u, false);
	glyphs(a, 22, 22, 4, all);
	a->rect(0, 150, SIZE, 160, all, 0xff00ff00u, false);
	a->rect(0, 0, SIZE, 40, all, 0x800000ffu, false);
	note(a->rounded(60, 60, 260, 120, r4, 0, all, 0xffffffffu, 0xffffffffu));
	a->rclip(NULL);
	a->rect(0, 0, 40, 40, all, 0xff808080u, false);
	a->end();
}

/* What the CPU leaves to the UI: a glyph scaled, a border. */
static void left_out(const struct api *a, uint32_t *buf)
{
	const float r8[4] = { 8, 8, 8, 8 };
	a->begin(buf, SIZE, SIZE, SIZE);
	a->rect(0, 0, SIZE, SIZE, all, 0xff204060u, true);
	glyphs(a, 10, 10, 8, all);
	note(a->texture(glyph[0], GW, GW, GH, PAINT_ALPHA, 100, 100, GW * 2, GH * 2, all, 0xffffffffu, false));
	glyphs(a, 10, 60, 8, all);
	note(a->rounded(150, 150, 250, 250, r8, 2, all, 0xff0000ffu, 0xffffffffu));
	a->rect(160, 160, 240, 240, all, 0x80ffffffu, false);
	a->end();
}

/* More quads and opacities than one frame takes. */
static void many(const struct api *a, uint32_t *buf)
{
	a->begin(buf, SIZE, SIZE, SIZE);
	a->rect(0, 0, SIZE, SIZE, all, 0, true);
	glyphs(a, 0, 0, 16 * 16, all);
	glyphs(a, 3, 5, 16 * 16, all);
	for (int i = 0; i < 150; i++)
		note(a->texture(img_pm, 160, 40, 30, PAINT_RGBA_PREMULTIPLIED, (i * 37) % 300, (i * 53) % 300, 40, 30, all,
				(uint32_t)(i % 2 ? 0x60 : 0xc0) * 0x01010101u, false));
	a->end();
}

/* More images than the atlas holds. */
static void crowded(const struct api *a, uint32_t *buf)
{
	a->begin(buf, SIZE, SIZE, SIZE);
	a->rect(0, 0, SIZE, SIZE, all, 0xff000000u, true);
	for (int i = 0; i < 40; i++)
		note(a->texture(big + i * 4 * 1, 640, 100, 100, PAINT_RGBA_PREMULTIPLIED, (i * 41) % 260, (i * 67) % 260, 100,
				100, all, 0xffffffffu, false));
	a->end();
}

/* A clock's tick: too little to take to the GPU. */
static void small(const struct api *a, uint32_t *buf)
{
	const int32_t r[4] = { 10, 10, 40, 40 };
	a->begin(buf, SIZE, SIZE, SIZE);
	a->rect(10, 10, 40, 40, r, 0, true);
	glyphs(a, 12, 12, 2, r);
	a->end();
}

/* The UI drawing rows itself between the GPU's calls. */
static void cpu_rows(const struct api *a, uint32_t *buf)
{
	a->begin(buf, SIZE, SIZE, SIZE);
	a->rect(0, 0, SIZE, SIZE, all, 0xff102030u, true);
	glyphs(a, 10, 10, 30, all);
	a->sync();
	for (int y = 100; y < 110; y++)
		for (int x = 20; x < 300; x++)
			buf[y * SIZE + x] = 0xff000000u | (uint32_t)(x * y);
	glyphs(a, 15, 95, 30, all);
	a->cpu(100, 110);
	a->rect(0, 105, SIZE, 108, all, 0x80000080u, false);
	a->end();
}

/* --- running them */

static void layer_fill(uint32_t *buf, bool noise)
{
	uint32_t s = seed;
	for (int i = 0; i < SIZE * SIZE; i++)
		buf[i] = noise ? premul(rnd()) : 0;
	seed = s;
}

static bool left_rendering;

/* Painted by the CPU alone into ref, then by the GPU into the layer, from
 * the same start: the same pixels, the same answers. */
static void same(const char *name, void (*paint)(const struct api *, uint32_t *), bool noise)
{
	bool soft_ret[64];
	layer_fill(mem.ref, noise);
	layer_fill(mem.layer, noise);
	memcpy(dram, mem.layer, sizeof dram);
	nreturned = 0;
	paint(&soft_api, mem.ref);
	memcpy(soft_ret, returned, sizeof soft_ret);
	unsigned n = nreturned;
	nreturned = 0;
	paint(&gpu_api, mem.layer);
	paint_finish();
	left_rendering = sgx_draw_busy();
	paint_wait();
	CHECK(!sgx_draw_busy(), "%s: a scene still rendering after paint_wait", name);
	CHECK(n == nreturned && !memcmp(soft_ret, returned, n), "%s: answers differ from the CPU's", name);
	CHECK(rows_format == 0x0d, "%s: rows format left at %02x", name, rows_format);
	unsigned diff = 0, first = 0;
	for (int i = 0; i < SIZE * SIZE; i++)
		if (mem.layer[i] != mem.ref[i] && !diff++)
			first = (unsigned)i;
	CHECK(!diff, "%s: %u pixels differ, first %u,%u: %08x, the CPU %08x", name, diff, first % SIZE, first / SIZE,
	      mem.layer[first], mem.ref[first]);
	/* What was painted holds every pixel with content, but in the bands
	 * the UI wrote itself. */
	struct paint_bounds k;
	CHECK(paint_bounds(mem.layer, &k), "%s: no bounds", name);
	CHECK(!paint_bounds(mem.layer, &k), "%s: the bounds were not taken", name);
	for (int y = 0; y < SIZE && !noise; y++) {
		int b = y / PAINT_BAND;
		if (k.scan >> b & 1)
			continue;
		for (int x = 0; x < SIZE; x++)
			if (mem.layer[y * SIZE + x] && (x < k.x0[b] || x >= k.x1[b])) {
				CHECK(false, "%s: content at %d,%d outside band %d's %d..%d", name, x, y, b, k.x0[b], k.x1[b]);
				y = SIZE;
				break;
			}
	}
	/* frame.c cleans the damaged rows once it has scanned them. */
	memcpy(dram, mem.layer, sizeof dram);
}

static unsigned frames_of(const char *name, void (*paint)(const struct api *, uint32_t *), bool noise)
{
	unsigned f0 = frames;
	same(name, paint, noise);
	return frames - f0;
}

static void reset_gpu(void)
{
	running = true;
	failed = refuse = false;
	frame_ok = true;
	paint_mode(PAINT_MODE_GPU);
}

int main(void)
{
	gpu_host_base = (uintptr_t)&mem;
	fill_inputs();
	paint_mode(PAINT_MODE_GPU);
	CHECK(paint_overlap_on(), "overlap on by default");
	paint_overlap(false);
	/* The small atlases, which the crowded repaints fill. */
	CHECK(paint_atlas(480, 480), "the small atlases");

	CHECK(frames_of("full", full, false) == 1, "a full repaint in one frame");
	CHECK(frames_of("partial", partial, true) >= 1, "a partial repaint on the GPU");
	CHECK(frames_of("rounded clip", rounded_clip, true) >= 3, "the corners of a rounded clip on the CPU");
	CHECK(frames_of("left out", left_out, false) == 3, "a frame on either side of each call left to the UI");
	unsigned f = frames_of("many", many, true);
	CHECK(f >= 2 && max_draws <= SGX_DRAW_MAX_DRAWS, "many quads in %u frames", f);
	CHECK(frames_of("crowded", crowded, false) >= 3, "the atlas started over");
	CHECK(frames_of("small", small, true) == 0, "a small repaint on the GPU");
	CHECK(frames_of("cpu rows", cpu_rows, false) == 2, "a sync between two frames");

	/* A frame the GPU refuses is painted by the CPU; the GPU stays on. */
	refuse = true;
	logs[0] = 0;
	same("refused", full, false);
	CHECK(strstr(logs, "does not take") && !halts && running, "a refusal: %s", logs);
	refuse = false;
	CHECK(frames_of("after a refusal", full, false) == 1, "the GPU after a refusal");

	/* A failure halts the GPU, and the CPU paints from then on, until the
	 * GPU is asked for again. */
	frame_ok = false;
	logs[0] = 0;
	same("failed", full, false);
	CHECK(halts == 1 && strstr(logs, "the CPU paints until"), "a failure: %u halts, %s", halts, logs);
	running = true;
	frame_ok = true;
	CHECK(frames_of("after a failure", full, false) == 0 && paint_gpu_lost(), "the GPU after a failure");
	reset_gpu();
	generation++;
	atlases_given = 0;
	CHECK(frames_of("after a restart", full, false) == 1 && !paint_gpu_lost(), "the GPU asked for again");

	/* With overlap, the same pixels; the last scene of a repaint renders
	 * on past paint_finish, and starting the atlas over takes the other
	 * one instead of waiting. */
	paint_overlap(true);
	CHECK(frames_of("overlap full", full, false) == 1 && left_rendering, "a repaint left rendering");
	CHECK(frames_of("overlap partial", partial, true) >= 1, "a partial repaint with overlap");
	CHECK(frames_of("overlap rounded clip", rounded_clip, true) >= 3, "a rounded clip with overlap");
	CHECK(frames_of("overlap left out", left_out, false) == 3, "calls left to the UI with overlap");
	f = frames_of("overlap many", many, true);
	CHECK(f >= 2, "many quads with overlap in %u frames", f);
	CHECK(frames_of("overlap crowded", crowded, false) >= 3 && atlases_given == 2, "the other atlas");
	/* Bigger atlases, from the GPU's next start, take all of them in one
	 * scene. */
	CHECK(!paint_atlas(512, 480) && !paint_atlas(500, 480) && !paint_atlas(960, 928), "an atlas the GPU does not take");
	CHECK(paint_atlas(480, 1440), "a big atlas");
	generation++;
	atlases_given = 0;
	CHECK(frames_of("crowded in big atlases", crowded, false) == 1, "a big atlas started over");
	CHECK(frames_of("full in big atlases", full, false) == 1, "a full repaint in big atlases");
	paint_atlas(480, 480);
	generation++;
	atlases_given = 0;
	CHECK(frames_of("overlap small", small, true) == 0, "a small repaint with overlap");
	CHECK(frames_of("overlap cpu rows", cpu_rows, false) == 2, "a sync with overlap");

	/* A failure found only when the scene is waited for: the CPU paints
	 * it again and the GPU is halted. */
	finish_ok = false;
	logs[0] = 0;
	unsigned h0 = halts;
	same("failed at the wait", full, false);
	CHECK(halts == h0 + 1 && strstr(logs, "the CPU paints until") && paint_gpu_lost(), "a failure at the wait: %s",
	      logs);
	finish_ok = true;
	reset_gpu();
	generation++;
	atlases_given = 0;
	CHECK(frames_of("overlap after a restart", full, false) == 1, "the GPU asked for again with overlap");
	paint_overlap(false);
	CHECK(frames_of("overlap off", full, false) == 1 && !left_rendering, "a repaint waited for without overlap");

	/* Off, soft or null: no frames. */
	running = false;
	CHECK(frames_of("gpu off", full, false) == 0, "frames with the GPU off");
	reset_gpu();
	paint_mode(PAINT_MODE_SOFT);
	CHECK(frames_of("soft", full, false) == 0, "frames while the CPU paints");
	paint_mode(PAINT_MODE_GPU);

	logs[0] = 0;
	paint_log();
	CHECK(strstr(logs, "paint gpu: ") && strstr(logs, "flush sync"), "the log: %s", logs);
	for (char *l = logs, *e; *l; l = e + 1) {
		e = strchr(l, '\n');
		if (!e)
			break;
		CHECK(e - l < 240, "a log line of %d characters", (int)(e - l));
	}

	if (failures) {
		printf("sgx_paint_test: %d failures\n", failures);
		return 1;
	}
	printf("sgx_paint_test: ok\n");
	return 0;
}
