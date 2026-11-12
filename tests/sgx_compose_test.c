/* The GPU compositor on the host, against a fake GPU: what a frame of
 * draw.h calls becomes as draws (positions flipped and turned, layers as
 * quads, indicators in slots and triangles in a palette of the compositor's
 * own texture), and that the CPU draws the same frame when the GPU is off,
 * fails or is given something it does not take. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "gpu/a3xx.h"
#include "gpu/soft.h"
#include "gpu/sgx.h"
#include "gpu/sgx_draw.h"
#include "gpu/sgx_compose.h"
#include "display/display.h"

/* unistd.h would clash with the firmware's reboot(). */
pid_t fork(void);
void _exit(int status) __attribute__((noreturn));

#define SIZE		SGX_DRAW_SIZE
#define PITCH		(SIZE * 4)
#define NMESH		300
#define NBG		12
#define NOPQ		6
#define AUX_GPU		0x80000000u
#define VERT_GPU	0x90000000u
#define MAP_GPU(i)	(0x40000000u + 0x10000000u * (i))

/* One block for everything the calls are given, named by 32-bit offsets
 * from gpu_host_base; the frame buffer and the layer are the regions. */
static struct {
	uint32_t frame[SIZE * SIZE] __attribute__((aligned(4096)));
	uint32_t layer[SIZE * SIZE] __attribute__((aligned(4096)));
	uint32_t ref[SIZE * SIZE] __attribute__((aligned(4096)));
	float mesh[NMESH * 8], bg[NBG * 8], opq[NOPQ * 8];
} mem __attribute__((aligned(4096)));
uintptr_t gpu_host_base;
#define ADDR(p)	((uint32_t)((uintptr_t)(p) - gpu_host_base))

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

/* --- the fake GPU */

static struct { uint32_t phys, size; } maps[4];
static unsigned nmaps, map_fail, starts, stops, halts, frame_calls, nlogs, cleans;
static bool running, failed, start_ok = true, stop_ok = true, frame_ok = true, refuse, refused;
static uint32_t generation = 1, setup_gen = 1;
static uint32_t now_ms, clean_size;
static const void *clean_buf;
static char lastlog[256];
static uint32_t aux_mem[SIZE * SIZE] __attribute__((aligned(4096)));
static bool aux_given;
static float vmem[16384];
static uint32_t vused, target_gpu;

static struct {
	uint32_t clear, target, n;
	struct sgx_draw d[SGX_DRAW_MAX_DRAWS];
	float v[sizeof vmem / 4];
	uint32_t aux[SIZE * SIZE];
	char bad[160];
} cap;

void rotation_buffer(uint32_t *buf, uint32_t stride) {}
uint32_t time_ms(void) { return now_ms; }
uint32_t time_us(void)
{
	static uint32_t t;
	return t += 7;
}

void log_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(lastlog, sizeof lastlog, fmt, ap);
	va_end(ap);
	nlogs++;
}

void dcache_clean_invalidate_range(const void *addr, uint32_t size)
{
	cleans++;
	clean_buf = addr;
	clean_size = size;
}

const float (*flatmesh_round_edge(void))[2]
{
	static float edge[FLATMESH_ROUND_SIDES][2];
	if (!edge[0][0]) {
		const float c = 0.99518473f, s = 0.09801714f;
		edge[0][0] = 0.5f;
		for (int k = 1; k < FLATMESH_ROUND_SIDES; k++) {
			edge[k][0] = edge[k - 1][0] * c - edge[k - 1][1] * s;
			edge[k][1] = edge[k - 1][0] * s + edge[k - 1][1] * c;
		}
	}
	return edge;
}

/* As sgx.c: a failure leaves the GPU off, and the start after it builds
 * everything again. */
bool sgx_start(void)
{
	starts++;
	if (failed)
		generation++;
	running = start_ok;
	failed = !start_ok;
	return start_ok;
}
bool sgx_stop(void)
{
	stops++;
	if (running && !stop_ok)
		failed = true;
	running = false;
	return !failed;
}
void sgx_halt(void)
{
	halts++;
	running = false;
	failed = true;
}
bool sgx_running(void) { return running; }
bool sgx_failed(void) { return failed; }
uint32_t sgx_generation(void) { return generation; }
bool sgx_draw_setup(void)
{
	if (setup_gen != generation) {
		nmaps = 0;
		aux_given = false;
		setup_gen = generation;
	}
	return running;
}

uint32_t sgx_draw_map(uint32_t phys, uint32_t size)
{
	if (nmaps == 4 || nmaps + 1 == map_fail || (phys | size) & 4095)
		return 0;
	maps[nmaps].phys = phys;
	maps[nmaps].size = size;
	return MAP_GPU(nmaps++);
}

uint32_t sgx_draw_alloc(uint32_t size, void **cpu)
{
	if (aux_given || size > sizeof aux_mem)
		return 0;
	aux_given = true;
	*cpu = aux_mem;
	return AUX_GPU;
}

void *sgx_draw_vertices(uint32_t bytes, uint32_t *gpu)
{
	if (vused + bytes > sizeof vmem)
		return NULL;
	void *p = (char *)vmem + vused;
	*gpu = VERT_GPU + vused;
	vused += (bytes + 15) & ~15u;
	return p;
}

void sgx_draw_discard(void) { vused = 0; }
void sgx_draw_target(uint32_t gpu_addr) { target_gpu = gpu_addr; }
const char *sgx_draw_failure(void) { return refused ? "fake refusal" : "fake failure"; }
bool sgx_draw_refused(void) { return refused; }

/* The rules of sgx_draw.h, noted for the test to look at. */
static void validate(const struct sgx_frame *f)
{
	cap.bad[0] = 0;
	if (!running)
		snprintf(cap.bad, sizeof cap.bad, "a frame while the GPU is off");
	for (unsigned i = 0; i < f->n && !cap.bad[0]; i++) {
		const struct sgx_draw *d = &f->draws[i];
		bool flat = d->kind == SGX_DRAW_FLAT;
		uint32_t bytes = d->count * (flat ? 24 : 16);
		if (!d->count || d->count % 3 || d->count > SGX_DRAW_MAX_VERTS)
			snprintf(cap.bad, sizeof cap.bad, "draw %u: %u vertices", i, d->count);
		else if (d->verts < VERT_GPU || d->verts - VERT_GPU + bytes > vused || (d->verts & 3))
			snprintf(cap.bad, sizeof cap.bad, "draw %u: vertices at %08x", i, d->verts);
		else if (flat && (i || d->blend))
			snprintf(cap.bad, sizeof cap.bad, "draw %u: flat (blend %d)", i, d->blend);
		else if (!flat && (!d->blend || d->linear || d->width != SIZE || d->height != SIZE))
			snprintf(cap.bad, sizeof cap.bad, "draw %u: textured blend %d linear %d %ux%u", i, d->blend, d->linear,
				 d->width, d->height);
		else if (!flat && (d->tex & 4095 || (d->tex != AUX_GPU && d->tex != MAP_GPU(1))))
			snprintf(cap.bad, sizeof cap.bad, "draw %u: texture at %08x", i, d->tex);
	}
}

bool sgx_draw_frame(const struct sgx_frame *f, struct sgx_times *t)
{
	frame_calls++;
	validate(f);
	cap.clear = f->clear;
	cap.n = f->n;
	cap.target = target_gpu;
	memcpy(cap.d, f->draws, f->n * sizeof *f->draws);
	memcpy(cap.v, vmem, vused);
	memcpy(cap.aux, aux_mem, sizeof aux_mem);
	vused = 0;
	t->ta_us = t->render_us = 100;
	refused = refuse;
	return frame_ok && !refuse;
}

/* --- what the calls are given */

static uint32_t rnd(void)
{
	static uint32_t s = 4242;
	s = s * 1664525u + 1013904223u;
	return s >> 8;
}

static uint32_t div255(uint32_t v)
{
	v += 128;
	return (v + (v >> 8)) >> 8;
}

static uint8_t byte_of(float v) { return (uint8_t)(int)((double)v * 255.0 + 0.5); }

static uint32_t texel(const float *c)
{
	return byte_of(c[0]) | byte_of(c[1]) << 8 | byte_of(c[2]) << 16 | (uint32_t)byte_of(c[3]) << 24;
}

static uint32_t *ind[2];

static void triangles(float *v, unsigned n, float alpha)
{
	for (unsigned t = 0; t < n / 3; t++) {
		float cx = (float)(rnd() % 200) / 100 - 1, cy = (float)(rnd() % 200) / 100 - 1;
		float col[4] = { (float)(rnd() % 256) / 255, (float)(rnd() % 256) / 255, (float)(rnd() % 256) / 255, alpha };
		for (unsigned k = 0; k < 3; k++) {
			float *o = v + (t * 3 + k) * 8;
			o[0] = cx + (float)(rnd() % 80) / 100 - 0.4f;
			o[1] = cy + (float)(rnd() % 80) / 100 - 0.4f;
			o[2] = 0;
			o[3] = 1;
			for (int i = 0; i < 4; i++)
				o[4 + i] = i < 3 && alpha < 1 ? col[i] * alpha : col[i];
		}
	}
}

static void fill_mem(void)
{
	triangles(mem.mesh, NMESH, 1.0f);
	triangles(mem.bg, NBG, 0.6f);
	triangles(mem.opq, NOPQ, 0.3f);
	for (int i = 0; i < SIZE * SIZE; i++) {
		uint32_t a = rnd() % 4 == 0 ? 0 : rnd() & 0xff;
		mem.layer[i] = a << 24 | div255((rnd() & 0xff) * a) << 16 | div255((rnd() & 0xff) * a) << 8 | div255((rnd() & 0xff) * a);
	}
}

static void fill_frame(uint32_t *f)
{
	for (int i = 0; i < SIZE * SIZE; i++)
		f[i] = 0xff000000u | (uint32_t)(i * 2654435761u) >> 8;
}

static const struct gpu_rect rects[3] = { { 0, 0, 320, 160 }, { 20, 170, 100, 50 }, { 200, 10, 60, 300 } };
#define LX	4
#define LY	8
static const uint8_t nc[2] = { 4, 4 };

struct api {
	void (*begin)(const struct gpu_target *);
	void (*blend)(bool);
	void (*turn)(float, float);
	void (*turn_vertices)(float *, unsigned, unsigned);
	void (*tris)(uint32_t, unsigned, unsigned, const uint8_t *, unsigned);
	void (*parts)(uint32_t, uint32_t, uint32_t, uint32_t, int, int, float, const struct gpu_rect *, unsigned);
	void (*at)(uint32_t, uint32_t, uint32_t, uint32_t, int, int, float);
	void (*round)(uint32_t, uint32_t, uint32_t, uint32_t, int, int, float);
	bool (*end)(void);
};
static const struct api gpu_api = { gpu_draw_begin, gpu_draw_blend, gpu_draw_turn, gpu_draw_turn_vertices,
				    gpu_draw_triangles, gpu_draw_layer_parts, gpu_draw_layer_at, gpu_draw_layer_round,
				    gpu_draw_end };
static const struct api soft_api = { soft_draw_begin, soft_draw_blend, soft_draw_turn, soft_draw_turn_vertices,
				     soft_draw_triangles, soft_draw_layer_parts, soft_draw_layer_at, soft_draw_layer_round,
				     soft_draw_end };

/* The turn a frame asks for, and how much of its triangles are turned. */
static struct scene {
	float c, s;
	bool call_turn, bg_turned;
	unsigned mesh_turned;
} sc = { 1, 0, true, true, NMESH };

static struct gpu_target frame_target(void) { return (struct gpu_target){ ADDR(mem.frame), SIZE, SIZE, PITCH, false }; }

/* A frame shaped like frame.c's. */
static bool full(const struct api *a, const struct gpu_target *t)
{
	a->begin(t);
	if (sc.call_turn)
		a->turn(sc.c, sc.s);
	if (sc.mesh_turned)
		a->turn_vertices(mem.mesh, sc.mesh_turned, 8);
	if (sc.bg_turned)
		a->turn_vertices(mem.bg, NBG, 8);
	a->tris(ADDR(mem.mesh), 32, 2, nc, NMESH);
	a->parts(ADDR(mem.layer), SIZE, SIZE, PITCH, LX, LY, 0.5f, rects, 3);
	a->at(ADDR(ind[0]), 32, 32, 128, 144, 12, 1.0f);
	a->at(ADDR(ind[1]), 32, 32, 128, 100, 280, 1.0f);
	a->blend(true);
	a->tris(ADDR(mem.bg), 32, 2, nc, NBG);
	a->blend(false);
	a->tris(ADDR(mem.opq), 32, 2, nc, NOPQ);
	a->round(ADDR(mem.layer), SIZE, SIZE, PITCH, 0, 0, 0.75f);
	return a->end();
}

/* A layer that fits no slot, over the mesh. */
static bool big_layer(const struct api *a, const struct gpu_target *t)
{
	a->begin(t);
	a->turn(1, 0);
	a->tris(ADDR(mem.mesh), 32, 2, nc, NMESH);
	a->at(ADDR(mem.layer), 64, 64, 256, 20, 30, 0.5f);
	return a->end();
}

/* No mesh to start the frame. */
static bool no_mesh(const struct api *a, const struct gpu_target *t)
{
	a->begin(t);
	a->turn(1, 0);
	a->at(ADDR(ind[0]), 32, 32, 128, 144, 12, 1.0f);
	return a->end();
}

static bool empty(const struct api *a, const struct gpu_target *t)
{
	a->begin(t);
	a->turn(1, 0);
	return a->end();
}

/* The same calls on the CPU alone, into the second buffer. */
static bool same_as_soft(bool (*frame)(const struct api *, const struct gpu_target *), const struct gpu_target *t, bool *ret)
{
	struct gpu_target r = *t;
	r.base = ADDR(mem.ref);
	fill_frame(mem.frame);
	fill_frame(mem.ref);
	frame(&soft_api, &r);
	*ret = frame(&gpu_api, t);
	return !memcmp(mem.frame, mem.ref, sizeof mem.frame);
}

static void setup(void)
{
	uint32_t size;
	void *pool = soft_pool(&size);
	const struct sgx_region r[] = { { ADDR(mem.frame), sizeof mem.frame }, { ADDR(mem.layer), sizeof mem.layer },
					{ ADDR(pool), size } };
	sgx_compose_regions(r, 3);
	for (int k = 0; k < 2; k++) {
		ind[k] = gpu_alloc(32 * 128, 128);
		for (int i = 0; i < 32 * 32; i++)
			ind[k][i] = rnd() | 1;
	}
}

/* --- the captured frame */

static bool near(float a, float b) { return a - b < 2e-6f && b - a < 2e-6f; }

/* A clip-space point as the GPU takes it: turned if asked, then flipped. */
static void clip(float *o, double x, double y, bool turn)
{
	if (turn) {
		double tx = x * sc.c + y * sc.s;
		y = y * sc.c - x * sc.s;
		x = tx;
	}
	o[0] = (float)x;
	o[1] = (float)-y;
}

static const float *verts(const struct sgx_draw *d) { return cap.v + (d->verts - VERT_GPU) / 4; }

/* The six vertices of the quad of a pixel rectangle sampling a texture
 * rectangle. */
static unsigned quad(const float *v, double x0, double y0, double x1, double y1, double u0, double t0, double u1, double t1)
{
	const double p[6][4] = { { x0, y0, u0, t0 }, { x1, y0, u1, t0 }, { x0, y1, u0, t1 },
				 { x1, y0, u1, t0 }, { x1, y1, u1, t1 }, { x0, y1, u0, t1 } };
	unsigned bad = 0;
	for (int i = 0; i < 6; i++) {
		float w[2];
		clip(w, 2 * p[i][0] / SIZE - 1, 1 - 2 * p[i][1] / SIZE, true);
		bad += !near(v[4 * i], w[0]) || !near(v[4 * i + 1], w[1]) || !near(v[4 * i + 2], (float)p[i][2]) ||
		       !near(v[4 * i + 3], (float)p[i][3]);
	}
	return bad;
}

/* Triangles drawn from the palette: three equal coordinates pointing at
 * a texel with the first vertex's colour. */
static unsigned palette(const struct sgx_draw *d, const float *src, unsigned first, bool opaque, bool turned)
{
	const float *v = verts(d);
	unsigned bad = 0;
	for (unsigned i = 0; i < d->count; i++) {
		const float *f = src + i * 8, *c = src + i / 3 * 24 + 4;
		float w[2], k[4] = { c[0], c[1], c[2], opaque ? 1.0f : c[3] };
		clip(w, f[0], f[1], turned);
		unsigned tx = (unsigned)(v[4 * i + 2] * SIZE), ty = (unsigned)(v[4 * i + 3] * SIZE);
		bad += !near(v[4 * i], w[0]) || !near(v[4 * i + 1], w[1]) || v[4 * i + 2] != v[4 * (i - i % 3) + 2] ||
		       v[4 * i + 3] != v[4 * (i - i % 3) + 3] || tx != (first + i / 3) % SIZE ||
		       ty != 32 + (first + i / 3) / SIZE || cap.aux[ty * SIZE + tx] != texel(k);
	}
	return bad;
}

static void check_gpu(const char *what)
{
	CHECK(!cap.bad[0], "%s: %s", what, cap.bad);
	CHECK(cap.n == 7 && cap.clear == 0xff000000u && cap.target == MAP_GPU(0), "%s: %u draws, clear %08x, target %08x", what, cap.n,
	      cap.clear, cap.target);
	if (cap.n != 7 || cap.bad[0])
		return;
	const struct sgx_draw *d = cap.d;
	const float *v = verts(&d[0]);
	unsigned bad = 0;
	CHECK(d[0].kind == SGX_DRAW_FLAT && d[0].count == NMESH && !d[0].blend, "%s: mesh draw", what);
	for (unsigned i = 0; i < NMESH; i++) {
		const float *f = mem.mesh + i * 8;
		float w[2];
		clip(w, f[0], f[1], i < sc.mesh_turned);
		bad += !near(v[6 * i], w[0]) || !near(v[6 * i + 1], w[1]) || memcmp(&v[6 * i + 2], f + 4, 16);
	}
	CHECK(!bad, "%s: %u mesh vertices differ", what, bad);

	CHECK(d[1].kind == SGX_DRAW_TEXTURED && d[1].tex == MAP_GPU(1) && d[1].alpha == 128 && d[1].count == 18, "%s: layer draw", what);
	bad = 0;
	for (int i = 0; i < 3; i++)
		bad += quad(verts(&d[1]) + 24 * i, LX + rects[i].x, LY + rects[i].y, LX + rects[i].x + rects[i].width,
			    LY + rects[i].y + rects[i].height, (double)rects[i].x / SIZE, (double)rects[i].y / SIZE,
			    (double)(rects[i].x + rects[i].width) / SIZE, (double)(rects[i].y + rects[i].height) / SIZE);
	CHECK(!bad, "%s: %u layer vertices differ", what, bad);

	const int ix[2][2] = { { 144, 12 }, { 100, 280 } };
	for (int k = 0; k < 2; k++) {
		const struct sgx_draw *e = &d[2 + k];
		CHECK(e->kind == SGX_DRAW_TEXTURED && e->tex == AUX_GPU && e->alpha == 255 && e->count == 6, "%s: indicator %d draw", what, k);
		bad = quad(verts(e), ix[k][0], ix[k][1], ix[k][0] + 32, ix[k][1] + 32, k * 32.0 / SIZE, 0, (k + 1) * 32.0 / SIZE, 32.0 / SIZE);
		CHECK(!bad, "%s: %u vertices of indicator %d differ", what, bad, k);
		bad = 0;
		for (int y = 0; y < 32; y++)
			for (int x = 0; x < 32; x++)
				bad += cap.aux[y * SIZE + k * 32 + x] != ind[k][y * 32 + x];
		CHECK(!bad, "%s: %u texels of indicator %d differ", what, bad, k);
	}

	CHECK(d[4].kind == SGX_DRAW_TEXTURED && d[4].tex == AUX_GPU && d[4].alpha == 255 && d[4].count == NBG, "%s: background draw", what);
	bad = palette(&d[4], mem.bg, 0, false, sc.bg_turned);
	CHECK(!bad, "%s: %u background vertices differ", what, bad);
	CHECK(d[5].kind == SGX_DRAW_TEXTURED && d[5].tex == AUX_GPU && d[5].alpha == 255 && d[5].count == NOPQ, "%s: opaque draw", what);
	bad = palette(&d[5], mem.opq, NBG / 3, true, false);
	CHECK(!bad, "%s: %u opaque vertices differ", what, bad);

	CHECK(d[6].kind == SGX_DRAW_TEXTURED && d[6].tex == MAP_GPU(1) && d[6].alpha == 191 && d[6].count == 192, "%s: round draw", what);
	const float (*edge)[2] = flatmesh_round_edge();
	bad = 0;
	for (int k = 0; k < FLATMESH_ROUND_SIDES; k++) {
		const float *a = edge[k], *b = edge[(k + 1) % FLATMESH_ROUND_SIDES];
		const double p[3][2] = { { 0.5, 0.5 }, { 0.5 + a[0], 0.5 + a[1] }, { 0.5 + b[0], 0.5 + b[1] } };
		for (int j = 0; j < 3; j++) {
			const float *w = verts(&d[6]) + 12 * k + 4 * j;
			float q[2];
			clip(q, 2 * p[j][0] - 1, 1 - 2 * p[j][1], true);
			bad += !near(w[0], q[0]) || !near(w[1], q[1]) || !near(w[2], (float)p[j][0]) || !near(w[3], (float)p[j][1]);
		}
	}
	CHECK(!bad, "%s: %u round vertices differ", what, bad);
}

/* --- cases */

/* The GPU is off at boot until it is turned on. */
static bool boot(void)
{
	return gpu_init() && !starts && !gpu_ready() && sgx_compose(true) && gpu_ready() && sgx_compose_on();
}

static void gpu_frame(void)
{
	setup();
	CHECK(boot(), "boot, then on");
	struct gpu_target t = frame_target();
	CHECK(full(&gpu_api, &t), "gpu_draw_end");
	CHECK(frame_calls == 1 && !nlogs && !halts, "%u frames, %u log lines", frame_calls, nlogs);
	check_gpu("upright");
}

static void turned(void)
{
	setup();
	CHECK(boot(), "boot, then on");
	struct gpu_target t = frame_target();
	static float before[sizeof mem.mesh / 4 + sizeof mem.bg / 4];
	memcpy(before, mem.mesh, sizeof mem.mesh);
	memcpy(before + NMESH * 8, mem.bg, sizeof mem.bg);
	sc = (struct scene){ 0, 1, true, true, 120 };
	CHECK(full(&gpu_api, &t), "gpu_draw_end");
	check_gpu("quarter turn");
	/* The first draw is where turned and unturned vertices meet. */
	const float *v = verts(&cap.d[0]), *m = mem.mesh;
	CHECK(near(v[0], m[1]) && near(v[1], m[0]) && near(v[6 * 200], m[200 * 8]) && near(v[6 * 200 + 1], -m[200 * 8 + 1]),
	      "turned and flipped vertices");
	CHECK(!memcmp(before, mem.mesh, sizeof mem.mesh) && !memcmp(before + NMESH * 8, mem.bg, sizeof mem.bg),
	      "the caller's vertices were changed");
	/* Which vertices are turned, and the turn, last one frame. */
	sc = (struct scene){ 0, 1, true, false, 0 };
	CHECK(full(&gpu_api, &t), "gpu_draw_end");
	check_gpu("layers turned, vertices not");
	sc = (struct scene){ 1, 0, false, false, 0 };
	CHECK(full(&gpu_api, &t), "gpu_draw_end");
	check_gpu("next frame, no turn");
	sc = (struct scene){ 0.6f, 0.8f, true, true, NMESH };
	CHECK(full(&gpu_api, &t), "gpu_draw_end");
	check_gpu("0.6, 0.8");
	CHECK(frame_calls == 4 && !nlogs, "%u frames, %u log lines", frame_calls, nlogs);
}

static void failing(void)
{
	setup();
	CHECK(boot(), "boot, then on");
	struct gpu_target t = frame_target();
	frame_ok = false;
	bool ret;
	bool same = same_as_soft(full, &t, &ret);
	CHECK(ret, "gpu_draw_end after a failed frame");
	CHECK(same, "the CPU's frame differs from soft's");
	CHECK(frame_calls == 1 && nlogs == 1 && halts == 1, "%u frames, %u log lines, %u halts", frame_calls, nlogs, halts);
	CHECK(strstr(lastlog, "fake failure"), "log line: %s", lastlog);
	CHECK(!gpu_ready() && !sgx_compose_on(), "still on after the failure");
	frame_ok = true;
	sc = (struct scene){ 0.6f, 0.8f, true, true, 100 };
	same = same_as_soft(full, &t, &ret);
	CHECK(ret && same && frame_calls == 1 && nlogs == 1, "next frame: %u GPU frames, %u log lines, equal %d", frame_calls, nlogs, same);
	unsigned s = starts;
	CHECK(gpu_init() && starts == s && !gpu_ready(), "gpu_init started the GPU again");
	gpu_suspend();
	CHECK(gpu_init() && starts == s && !gpu_ready(), "gpu_init after sleep started the GPU again");
	CHECK(nlogs == 1, "%u log lines", nlogs);
	/* By hand it is tried again, from scratch. */
	CHECK(sgx_compose(true) && gpu_ready() && generation == 2 && nmaps == 3 && aux_given, "sgx_compose(true) after a failure");
	CHECK(full(&gpu_api, &t) && frame_calls == 2, "a GPU frame after the retry");
	check_gpu("after the retry");
}

static void start_failing(void)
{
	setup();
	struct gpu_target t = frame_target();
	bool ret;
	start_ok = false;
	CHECK(gpu_init() && !sgx_compose(true) && !gpu_ready() && halts == 1, "a start that fails: %u halts", halts);
	CHECK(same_as_soft(full, &t, &ret) && ret && !frame_calls, "a frame after it");
	start_ok = true;
	gpu_suspend();
	unsigned s = starts;
	CHECK(gpu_init() && starts == s && !gpu_ready(), "wake tried again");
	/* A map that fails halts the GPU as well. */
	map_fail = 2;
	CHECK(!sgx_compose(true) && halts == 2 && !running, "a map that fails: %u halts", halts);
	map_fail = 0;
	CHECK(sgx_compose(true) && gpu_ready() && nmaps == 3, "on again: %u maps", nmaps);
	CHECK(full(&gpu_api, &t) && frame_calls == 1, "a GPU frame");
	check_gpu("after the start failures");
	/* A stop that fails at sleep keeps the GPU off after waking. */
	stop_ok = false;
	gpu_suspend();
	stop_ok = true;
	s = starts;
	CHECK(gpu_init() && starts == s && !gpu_ready() && !sgx_compose_on(), "wake after a failed stop");
	CHECK(same_as_soft(full, &t, &ret) && ret && frame_calls == 1, "a frame after it");
}

static void switching(void)
{
	setup();
	struct gpu_target t = frame_target();
	bool ret;
	CHECK(boot() && !sgx_compose(false) && !sgx_compose_on() && !gpu_ready() && stops == 1, "off: stops %u", stops);
	CHECK(same_as_soft(full, &t, &ret) && ret && !frame_calls, "a frame with the GPU off: %u on the GPU", frame_calls);
	CHECK(sgx_compose(true) && sgx_compose_on() && gpu_ready(), "back on");
	CHECK(full(&gpu_api, &t) && frame_calls == 1, "a frame back on the GPU");
	check_gpu("back on");
	gpu_suspend();
	CHECK(stops == 2, "sleep did not stop the GPU");
	CHECK(full(&gpu_api, &t) && frame_calls == 1, "a frame while asleep");
	CHECK(gpu_init() && gpu_ready(), "wake");
	CHECK(full(&gpu_api, &t) && frame_calls == 2, "a frame after waking");
	check_gpu("after waking");
	CHECK(!sgx_compose(false), "off again");
	gpu_suspend();
	unsigned s = starts;
	CHECK(gpu_init() && starts == s && !gpu_ready() && !sgx_compose_on(), "wake with the GPU off by hand started it");
	CHECK(same_as_soft(full, &t, &ret) && ret && frame_calls == 2 && !nlogs && !halts, "frame on the CPU: %u on the GPU", frame_calls);
}

static void odd(void)
{
	setup();
	CHECK(boot(), "boot, then on");
	bool ret;
	struct gpu_target t = frame_target(), small = { ADDR(mem.frame), 96, 80, 384, false };
	CHECK(same_as_soft(full, &small, &ret) && ret, "a 96x80 target");
	CHECK(!frame_calls && nlogs == 1, "%u GPU frames, %u log lines after a small target", frame_calls, nlogs);
	CHECK(same_as_soft(big_layer, &t, &ret) && ret, "a layer of 64x64");
	CHECK(!frame_calls && nlogs == 2 && !halts, "%u GPU frames, %u log lines after a big layer", frame_calls, nlogs);
	CHECK(same_as_soft(no_mesh, &t, &ret) && ret, "a frame without a mesh");
	CHECK(same_as_soft(empty, &t, &ret) && ret, "an empty frame");
	CHECK(!frame_calls && nlogs == 3 && !halts && gpu_ready(), "%u GPU frames, %u log lines, %u halts", frame_calls, nlogs, halts);
	/* Each refused frame gives its vertices back. */
	for (int i = 0; i < 200; i++)
		CHECK(big_layer(&gpu_api, &t), "a layer of 64x64, %d", i);
	CHECK(!frame_calls && nlogs == 3 && !halts, "%u GPU frames, %u log lines after the big layers", frame_calls, nlogs);
	CHECK(full(&gpu_api, &t) && frame_calls == 1 && gpu_ready(), "a normal frame afterwards");
	check_gpu("after the CPU's frames");
	/* A frame the driver refuses before the GPU sees it: the CPU draws
	 * it, and the GPU stays. */
	refuse = true;
	for (int i = 0; i < 50; i++)
		CHECK(same_as_soft(full, &t, &ret) && ret, "a refused frame, %d", i);
	refuse = false;
	CHECK(frame_calls == 51 && nlogs == 4 && !halts && gpu_ready() && strstr(lastlog, "fake refusal"),
	      "%u GPU frames, %u log lines, %u halts", frame_calls, nlogs, halts);
	CHECK(full(&gpu_api, &t) && frame_calls == 52, "a GPU frame after the refusals");
	check_gpu("after the refusals");
	/* Not a failure: sleep and wake keep the GPU. */
	gpu_suspend();
	CHECK(gpu_init() && gpu_ready(), "wake after frames the GPU did not take");
}

/* More calls than are kept: the GPU still draws the frame, but the CPU
 * cannot draw it again when the GPU fails. */
static bool many(const struct api *a, const struct gpu_target *t)
{
	a->begin(t);
	a->turn(1, 0);
	a->tris(ADDR(mem.mesh), 32, 2, nc, NMESH);
	for (int i = 0; i < 60; i++)
		a->blend(i & 1);
	a->at(ADDR(ind[0]), 32, 32, 128, 144, 12, 1.0f);
	return a->end();
}

static void unkept(void)
{
	setup();
	CHECK(boot(), "boot, then on");
	struct gpu_target t = frame_target();
	CHECK(many(&gpu_api, &t) && frame_calls == 1 && !cap.bad[0], "a frame of 63 calls: %s", cap.bad);
	CHECK(cap.n == 2 && cap.d[0].kind == SGX_DRAW_FLAT && cap.d[1].kind == SGX_DRAW_TEXTURED,
	      "%u draws", cap.n);
	frame_ok = false;
	fill_frame(mem.frame);
	fill_frame(mem.ref);
	CHECK(!many(&gpu_api, &t), "gpu_draw_end after a failure of a frame the CPU cannot draw");
	CHECK(halts == 1 && nlogs == 2 && !gpu_ready(), "%u halts, %u log lines", halts, nlogs);
	CHECK(!memcmp(mem.frame, mem.ref, sizeof mem.frame), "the frame buffer was changed");
	bool ret;
	CHECK(same_as_soft(full, &t, &ret) && ret && frame_calls == 2, "the next frame");
	/* frame.c no longer uses the compositor, so it is not turned on. */
	unsigned s = starts;
	CHECK(!sgx_compose(true) && starts == s && gpu_init() && starts == s, "on after a lost frame");
}

static void sync(void)
{
	setup();
	char buf[64];
	gpu_draw_cpu_sync(buf, sizeof buf);
	CHECK(!cleans, "a sync before the GPU was started");
	CHECK(boot(), "boot, then on");
	gpu_draw_cpu_sync(buf, sizeof buf);
	CHECK(cleans == 1 && clean_buf == buf && clean_size == sizeof buf, "sync with the GPU on: %u cleans", cleans);
	sgx_compose(false);
	gpu_draw_cpu_sync(buf, sizeof buf);
	CHECK(cleans == 1, "sync with the GPU off: %u cleans", cleans);
	sgx_compose(true);
	gpu_draw_cpu_sync(buf, sizeof buf);
	CHECK(cleans == 2, "sync with the GPU on again: %u cleans", cleans);
}

/* Each in a process of its own: the compositor's state is static. */
static void run(const char *name, void (*fn)(void))
{
	fflush(stdout);
	pid_t pid = fork();
	if (!pid) {
		failures = 0;
		fn();
		fflush(stdout);
		_exit(failures ? 1 : 0);
	}
	int st;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) {
		printf("FAIL %s\n", name);
		failures++;
	} else
		printf("%s: ok\n", name);
}

int main(void)
{
	uint32_t psize;
	uintptr_t pool = (uintptr_t)soft_pool(&psize), lo = pool < (uintptr_t)&mem ? pool : (uintptr_t)&mem;
	uintptr_t hi = pool + psize > (uintptr_t)&mem + sizeof mem ? pool + psize : (uintptr_t)&mem + sizeof mem;
	gpu_host_base = (lo & ~(uintptr_t)4095) - 4096;
	if (hi - gpu_host_base > 0xf0000000u) {
		printf("sgx_compose_test: memory too far apart for 32-bit addresses\n");
		return 1;
	}
	fill_mem();
	run("GPU frame", gpu_frame);
	run("turning", turned);
	run("GPU failure", failing);
	run("start and stop failures", start_failing);
	run("switching", switching);
	run("what the GPU does not take", odd);
	run("calls too many to keep", unkept);
	run("cache sync", sync);
	if (failures)
		return 1;
	printf("sgx_compose_test: ok\n");
	return 0;
}
