/* Where the Qualcomm watches' drawing calls go (qcom_compose.c): to the
 * Adreno while it starts and passes its selftest, to the CPU's drawing
 * from the first failure on, and then for good. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asteroid.h"
#include "gpu/adreno.h"
#include "gpu/a3xx.h"
#include "gpu/soft.h"
#include "gpu/qcom_compose.c"

#define W 16
#define H 8

uintptr_t gpu_host_base;
#define ADDR_OF(p)	((uint32_t)((uintptr_t)(p) - gpu_host_base))
void rotation_buffer(uint32_t *buf, uint32_t stride) {}

static uint32_t frame[H * W], layer[H * W];
static char lastlog[128];
static unsigned nlogs;
static bool init_ok, selftest_ok;
static unsigned inits, selftests, suspends, calls;
static char marker[64];

void log_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(lastlog, sizeof lastlog, fmt, ap);
	va_end(ap);
	nlogs++;
}

bool adreno_init(void)
{
	inits++;
	return init_ok;
}

bool adreno_selftest(void)
{
	selftests++;
	return selftest_ok;
}

void adreno_suspend(void) { suspends++; }
bool adreno_ready(void) { return init_ok && selftest_ok; }
void *adreno_alloc(uint32_t size, uint32_t align)
{
	calls++;
	return marker;
}

#define VOID_STUB(name, args) void name args { calls++; }
#define BOOL_STUB(name, args) bool name args { calls++; return true; }
VOID_STUB(adreno_draw_begin, (const struct gpu_target *t))
BOOL_STUB(adreno_draw_program, (struct gpu_shader *vs, struct gpu_shader *fs))
VOID_STUB(adreno_draw_consts, (int stage, unsigned first_vec4, const float *v, unsigned nvec4))
VOID_STUB(adreno_draw_texture, (int unit, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch_bytes))
VOID_STUB(adreno_draw_texture_format, (int unit, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, enum gpu_tex format))
VOID_STUB(adreno_draw_invalidate_textures, (void))
VOID_STUB(adreno_draw_blend, (bool premultiplied_over))
VOID_STUB(adreno_draw_triangles, (uint32_t vbo_addr, unsigned stride, unsigned nattrs, const uint8_t *attr_ncomp, unsigned nverts))
VOID_STUB(adreno_draw_layer, (uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes))
VOID_STUB(adreno_draw_layer_at, (uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha))
VOID_STUB(adreno_draw_layer_parts, (uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha, const struct gpu_rect *parts, unsigned n))
VOID_STUB(adreno_draw_layer_round, (uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha))
VOID_STUB(adreno_draw_turn, (float c, float s))
VOID_STUB(adreno_draw_turn_vertices, (float *v, unsigned n, unsigned stride))
VOID_STUB(adreno_draw_cpu_sync, (void *buf, uint32_t bytes))
VOID_STUB(adreno_draw_submit, (void))
BOOL_STUB(adreno_draw_wait, (void))
BOOL_STUB(adreno_draw_end, (void))
VOID_STUB(adreno_paint_rounded_clip, (const float *clip))
BOOL_STUB(adreno_paint_begin, (uint32_t *layer, uint32_t width, uint32_t height, uint32_t stride_px))
VOID_STUB(adreno_paint_rect, (float x0, float y0, float x1, float y1, const int32_t clip[4], uint32_t color, bool copy))
BOOL_STUB(adreno_paint_texture, (const uint8_t *data, uint32_t stride, uint32_t width, uint32_t height, int format, int dx, int dy, int dw, int dh, const int32_t clip[4], uint32_t tint, bool colorize))
BOOL_STUB(adreno_paint_rounded, (float x0, float y0, float x1, float y1, const float radii[4], float border, const int32_t clip[4], uint32_t fill, uint32_t edge))
VOID_STUB(adreno_paint_cpu, (uint32_t y0, uint32_t y1))
VOID_STUB(adreno_paint_sync, (void))
VOID_STUB(adreno_paint_end, (void))
VOID_STUB(adreno_paint_finish, (void))

static void check(bool ok, const char *what)
{
	if (!ok) {
		printf("qcom_compose_test: %s\n", what);
		exit(1);
	}
}

static void reset(bool init, bool selftest)
{
	cpu = false;
	init_ok = init;
	selftest_ok = selftest;
	inits = selftests = suspends = calls = nlogs = 0;
	memset(layer, 0, sizeof layer);
}

/* Every kind of call: where it went shows in calls and in the layer. */
static void draw(void)
{
	struct gpu_target t = { ADDR_OF(frame), W, H, W * 4, false };
	const int32_t clip[4] = { 0, 0, W, H };
	uint8_t texel[4] = { 0 };
	check(gpu_alloc(16, 16) != NULL, "alloc");
	gpu_draw_begin(&t);
	gpu_draw_blend(false);
	gpu_draw_layer_at(ADDR_OF(layer), W, H, W * 4, 0, 0, 1.0f);
	gpu_draw_end();
	check(paint_begin(layer, W, H, W), "paint_begin");
	paint_rect(0, 0, 4, 4, clip, 0xff0000ffu, true);
	paint_texture(texel, 4, 1, 1, PAINT_RGBA, 0, 0, 1, 1, clip, 0xffffffffu, false);
	paint_end();
	paint_finish();
}

static void on_adreno(void)
{
	reset(true, true);
	check(gpu_init() && gpu_selftest() && gpu_ready(), "a GPU that works");
	check(!nlogs && !suspends, "a GPU that works is not told about");
	draw();
	check(calls == 10 && !layer[0], "the calls of a GPU that works go to it");
	check(gpu_alloc(16, 16) == marker, "memory is the GPU's");
	gpu_suspend();
	check(suspends == 1, "sleep");
	check(gpu_init() && inits == 2 && !nlogs, "wake starts the GPU again");
}

static void on_cpu(const char *why)
{
	check(nlogs == 1 && !strcmp(lastlog, "gpu: composing on the CPU\n"), why);
	check(!gpu_ready(), "not ready on the CPU");
	unsigned c = calls;
	draw();
	check(calls == c, "the calls of a failed GPU go to the CPU");
	check(layer[0] == 0xff0000ffu && !layer[W * 4], "the CPU paints the layer");
	uint32_t size;
	uint8_t *pool = soft_pool(&size), *p = gpu_alloc(16, 16);
	check(p >= pool && p < pool + size, "memory is the CPU's");
	/* After sleep the GPU is not tried again. */
	unsigned i = inits;
	gpu_suspend();
	check(gpu_init() && gpu_selftest() && inits == i && nlogs == 1, "wake stays on the CPU");
}

int main(void)
{
	gpu_host_base = (uintptr_t)&frame < (uintptr_t)&layer ? (uintptr_t)&frame : (uintptr_t)&layer;
	on_adreno();

	reset(false, false);
	check(gpu_init() && gpu_selftest(), "a GPU that does not start leaves the CPU's selftest");
	check(selftests == 0, "no selftest of a GPU that did not start");
	on_cpu("a GPU that does not start");

	reset(true, false);
	check(gpu_init() && !nlogs, "a GPU that starts");
	check(gpu_selftest(), "a GPU that fails its selftest leaves the CPU's");
	check(suspends == 1, "a GPU that fails its selftest is turned off");
	on_cpu("a GPU that fails its selftest");

	/* A GPU that does not come back after sleep. */
	reset(true, true);
	check(gpu_init() && gpu_selftest() && gpu_ready(), "up first");
	gpu_suspend();
	init_ok = false;
	check(gpu_init(), "wake");
	on_cpu("a GPU that does not come back");

	printf("qcom_compose_test: ok\n");
	return 0;
}
