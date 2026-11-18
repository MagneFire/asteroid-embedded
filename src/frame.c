/* Drawing a frame: the FlatMesh background, on the GPU when there is one,
 * with the UI's panels over it. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "gpu/a3xx.h"
#include "gpu/draw.h"
#include "gpu/paint.h"
#include "gpu/shaders.h"
#ifdef SGX_BASE
#include "gpu/sgx_compose.h"
#endif
#include "ui/ui.h"
#include "ui/stack.h"
#include "ui/rotation.h"
#include "power.h"
#include "frame.h"
#include "sched.h"

#define MESH_STEP_MS 90

static int dot_x = -1, dot_y = -1;
static bool have_ui, have_gpu, gpu_off, woke;
static uint32_t gpu_frames, gpu_us, gpu_vert_us, gpu_fail;
static uint32_t mesh_us, layer_us, end_us, flip_us;
#ifdef SGX_BASE
static bool mesh_reuse = true;
static uint32_t mesh_kept;

void frame_mesh_reuse(bool on) { mesh_reuse = on; }
bool frame_mesh_reuse_on(void) { return mesh_reuse; }
#endif
/* How long after the last thing moved the processor may slow down. */
#define BUSY_MS	300
static uint32_t waited_us;
static uint32_t busy_t, idle_passes;
static uint32_t *const layers = (uint32_t *)(uintptr_t)UI_LAYER_BASE;
static uint32_t start_ms, shown_step = UINT32_MAX, hold_t;
static uint32_t ui_frames, ui_us, ui_repaints, ui_repaint_us;

static uint32_t mesh_t, app_center = 0x888888, app_outer, shown_center = 0x888888, shown_outer;
/* Under a still, opaque app the screen mesh is drawn in the app's
 * colours instead of a second mesh over it. */
static bool one_mesh;

/* An open app's own mesh in its colours, under it as it moves and fades. */
static void app_background(int x, int y, float alpha)
{
	if (one_mesh)
		return;
	static float *vbo;
	static unsigned n;
	static struct { uint32_t t, center, outer; int x, y; float alpha; } last;
	if (!vbo)
		vbo = gpu_alloc(FLATMESH_GPU_ROUND_VERTICES * 32, 64);
	/* The mesh only moves every MESH_STEP_MS. The Adreno turns the
	 * vertices in place, so a turned picture is built anew each frame. */
	float c, s;
	rotation_turn(time_ms(), &c, &s);
	bool same = n && c == 1.0f && s == 0.0f && last.t == mesh_t && last.center == app_center &&
		    last.outer == app_outer && last.x == x && last.y == y && last.alpha == alpha;
	if (!same) {
		n = flatmesh_vertices_gpu_look(vbo, mesh_t, app_center, app_outer, (float)x / fb.width,
					       (float)y / fb.height, alpha);
		last.t = mesh_t, last.center = app_center, last.outer = app_outer;
		last.x = x, last.y = y, last.alpha = c == 1.0f && s == 0.0f ? alpha : -1.0f;
	}
	gpu_draw_turn_vertices(vbo, n, 8);
	gpu_draw_program(&shader_c_vs, &shader_c_fs);
	gpu_draw_blend(alpha < 1.0f);
	gpu_draw_triangles((uint32_t)(uintptr_t)vbo, 32, 2, (const uint8_t[]){4, 4}, n);
	gpu_draw_blend(false);
}

#ifdef SGX_BASE
/* With paint overlap the GPU may still paint the damaged layers when
 * the repaint is done, so the scan of their pixels waits until the
 * compositor needs it. With bands from what was painted (paint_bounds)
 * only the bands the UI wrote itself are scanned, and no wait is needed
 * without them. */
static struct {
	bool any, damaged[UI_PANELS];
	uint32_t rect[UI_PANELS][4];
} later;
static int bands_mode = FRAME_BANDS_PAINTED;
static uint32_t bands_checked, bands_outside, bands_waits;

void frame_bands(int mode) { bands_mode = mode; }
int frame_bands_now(void) { return bands_mode; }

struct known {
	bool ok;
	struct paint_bounds b;
};

/* Whether the damage of a panel needs its pixels read. */
static bool needs_pixels(const struct known *k, const uint32_t *r)
{
	if (!bands_mode || !k->ok || bands_mode == FRAME_BANDS_VERIFY)
		return true;
	for (uint32_t b = r[1] / PAINT_BAND; b < PAINT_BANDS && b * PAINT_BAND < r[1] + r[3]; b++)
		if (k->b.scan >> b & 1)
			return true;
	return false;
}

static void damage_panel(unsigned p, const uint32_t *r, const struct known *k)
{
	uint32_t *layer = layers + p * (UI_LAYER_STRIDE / 4);
	if (bands_mode && k->ok) {
		unsigned checked = 0;
		bands_outside += stack_damage_known(p, r[0], r[1], r[2], r[3], &k->b, bands_mode == FRAME_BANDS_VERIFY,
						    &checked);
		bands_checked += checked;
	} else
		stack_damage(p, r[0], r[1], r[2], r[3]);
	if (have_gpu)
		for (uint32_t y = r[1]; y < r[1] + r[3]; y++)
			dcache_clean_range(layer + y * fb.stride + r[0], r[2] * 4);
}

static void known_of(unsigned p, struct known *k)
{
	k->ok = paint_bounds(layers + p * (UI_LAYER_STRIDE / 4), &k->b);
}

static void damage_later(void)
{
	if (!later.any)
		return;
	later.any = false;
	static struct known k[UI_PANELS];
	bool wait = false;
	for (unsigned p = 0; p < UI_PANELS; p++)
		if (later.damaged[p]) {
			known_of(p, &k[p]);
			wait |= needs_pixels(&k[p], later.rect[p]);
		}
	if (wait) {
		paint_wait();
		bands_waits++;
	}
	for (unsigned p = 0; p < UI_PANELS; p++)
		if (later.damaged[p])
			damage_panel(p, later.rect[p], &k[p]);
}
#endif

static uint32_t rgb(struct ui_rgb c)
{
	return (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | c.b;
}

static void gpu_flatmesh(uint32_t *buf, uint32_t t_ms, bool with_layer)
{
	static float *vbo;
	if (!vbo)
		vbo = gpu_alloc(FLATMESH_GPU_VERTICES * 32, 64);
	uint32_t t0 = time_us();
	mesh_t = t_ms;
#ifdef SGX_BASE
	float c, s;
	rotation_turn(time_ms(), &c, &s);
	/* The mesh only moves every MESH_STEP_MS, so its vertices are kept
	 * while it, the view and the colours stay; not when turned, as the
	 * turn may be applied to them in place. */
	static struct { uint32_t t, look; unsigned n; bool valid; } kept;
	bool straight = c == 1.0f && s == 0.0f;
	unsigned n;
	if (mesh_reuse && kept.valid && straight && kept.t == t_ms && kept.look == flatmesh_look()) {
		n = kept.n;
		mesh_kept++;
	} else {
		n = flatmesh_vertices_gpu(vbo, t_ms);
		kept.t = t_ms;
		kept.look = flatmesh_look();
		kept.n = n;
	}
	kept.valid = straight;
#else
	unsigned n = flatmesh_vertices_gpu(vbo, t_ms);
#endif
	struct gpu_target t = { (uint32_t)(uintptr_t)buf, fb.width, fb.height, fb.stride * 4, false };
	gpu_draw_cpu_sync(buf, fb.stride * fb.height * 4);
	uint32_t t1 = time_us();
	gpu_draw_begin(&t);
#ifndef SGX_BASE
	float c, s;
	rotation_turn(time_ms(), &c, &s);
#endif
	gpu_draw_turn(c, s);
	/* The frame-filling quad in front of the mesh stays put. */
	unsigned fill = n - FLATMESH_GPU_MESH;
	gpu_draw_turn_vertices(vbo + fill * 8, n - fill, 8);
	gpu_draw_program(&shader_c_vs, &shader_c_fs);
	gpu_draw_triangles((uint32_t)(uintptr_t)vbo, 32, 2, (const uint8_t[]){4, 4}, n);
	uint32_t t2 = time_us();
#ifdef SGX_BASE
	damage_later();
#endif
	if (with_layer)
		stack_compose(app_background);
	uint32_t t3 = time_us();
	mesh_us += t2 - t1;
	layer_us += t3 - t2;
	if (!gpu_draw_end()) {
		if (gpu_fail++ == 0)
			log_printf("gpu: draw did not complete\n");
		have_gpu = false;
	}
	gpu_draw_cpu_sync(buf, fb.stride * fb.height * 4);
	end_us += time_us() - t3;
	gpu_us += time_us() - t1;
	gpu_vert_us += t1 - t0;
	gpu_frames++;
}


static void draw_dot(uint32_t *buf)
{
	if (dot_x < 0)
		return;
	for (int dy = -6; dy <= 6; dy++)
		for (int dx = -6; dx <= 6; dx++) {
			int x = dot_x + dx, y = dot_y + dy;
			if (x >= 0 && y >= 0 && x < (int)fb.width && y < (int)fb.height && dx * dx + dy * dy <= 36)
				buf[y * fb.stride + x] = 0x00ffffff;
		}
}

/* The mesh fades from its grey to the colours of the launcher page on
 * screen as the launcher comes in, t from 0 to 1. */
static void mesh_colors(float t)
{
	uint32_t lc = 0x888888, lo = 0, c = 0, o = 0;
	struct ui_colors lcs;
	if (t > 0.0f && ui_launcher_colors(&lcs)) {
		lc = rgb(lcs.center);
		lo = rgb(lcs.outer);
	}
	for (int s = 0; s < 24; s += 8) {
		c |= (uint32_t)((float)(lc >> s & 0xff) * t + (float)(0x88) * (1 - t)) << s;
		o |= (uint32_t)((float)(lo >> s & 0xff) * t) << s;
	}
	if (c != shown_center || o != shown_outer) {
		shown_center = c;
		shown_outer = o;
		flatmesh_colors(c, o);
	}
}

void frame_init(bool ui, bool gpu, uint32_t start)
{
	have_ui = ui;
	have_gpu = gpu;
	start_ms = start;
}

/* Something is asked of the watch: full speed from now, not from the
 * frame that shows it. */
void frame_busy(uint32_t now)
{
	busy_t = now | 1;
	cpu_clock_idle(false);
}

void frame_touch_dot(int x, int y)
{
	dot_x = x;
	dot_y = y;
}

void frame_app_colors(const struct ui_colors *c)
{
	app_center = c ? rgb(c->center) : 0x888888;
	app_outer = c ? rgb(c->outer) : 0;
}

/* The GPU goes off with the screen and starts from scratch after it. */
void frame_sleep(void)
{
	if (have_gpu) {
		gpu_suspend();
		gpu_off = true;
	}
#ifdef SGX_BASE
	/* The console can start the SGX after frames stopped using it. */
	else
		gpu_suspend();
#endif
}

void frame_wake(void)
{
	if (gpu_off) {
		uint32_t g0 = time_us();
		have_gpu = gpu_init();
		gpu_off = false;
		log_printf("gpu: resumed in %lu us\n", time_us() - g0);
	}
	woke = true;
}

void frame_hold(uint32_t ms)
{
	hold_t = (time_ms() + ms) | 1;
}

/* Awake, a frame is drawn when the UI repainted, the stack moves
 * or animates, or the FlatMesh takes its next step: every 90 ms,
 * as qml-asteroid's FlatMesh timer. In between the CPU waits for
 * an interrupt, at the latest the next 1 ms tick. */
void frame_draw(uint32_t now)
{
	waited_us = 0;
	if (hold_t) {
		if ((int32_t)(hold_t - now) > 0)
			return;
		hold_t = 0;
		shown_step = UINT32_MAX;
	}
	uint32_t *buf = display_backbuffer(), step = (now - start_ms) / MESH_STEP_MS;
	bool frame = !have_ui || woke || step != shown_step;
	if (have_ui) {
#ifdef SGX_BASE
		uint32_t u0 = time_us();
#else
		uint32_t u0 = time_us(), x, y, w, h;
#endif
		bool repainted = false;
		int sx, sy;
		stack_update(now);
		if (stack_moving())
			power_activity(now);
		ui_tick();
		uint32_t damage[UI_PANELS][4];
		bool damaged[UI_PANELS] = { false };
		for (unsigned p = 0; p < UI_PANELS; p++)
			if ((p != UI_PANEL_OVERLAY || !stack_overlay_frozen()) &&
			    ui_update(p, &damage[p][0], &damage[p][1], &damage[p][2], &damage[p][3]))
				damaged[p] = repainted = true;
		if (repainted)
			paint_finish();
#ifdef SGX_BASE
		if (repainted && paint_overlap_on()) {
			later.any = true;
			memcpy(later.damaged, damaged, sizeof damaged);
			memcpy(later.rect, damage, sizeof damage);
		} else {
			for (unsigned p = 0; p < UI_PANELS; p++) {
				if (!damaged[p])
					continue;
				struct known k;
				known_of(p, &k);
				damage_panel(p, damage[p], &k);
			}
		}
#else
		for (unsigned p = 0; p < UI_PANELS; p++) {
			if (!damaged[p])
				continue;
			uint32_t *layer = layers + p * (UI_LAYER_STRIDE / 4);
			x = damage[p][0], y = damage[p][1], w = damage[p][2], h = damage[p][3];
			stack_damage(p, x, y, w, h);
			if (have_gpu)
				for (uint32_t r = y; r < y + h; r++)
					dcache_clean_range(layer + r * fb.stride + x, w * 4);
		}
#endif
		bool moving = repainted || stack_animating(now) || rotation_animating(now) || stack_moving();
		frame |= repainted || stack_animating(now) || rotation_animating(now);
		if (moving || woke)
			busy_t = now | 1;
		else if (busy_t && now - busy_t >= BUSY_MS)
			busy_t = 0;
		cpu_clock_idle(!busy_t);
		idle_passes += !busy_t;
		if (repainted) {
			ui_repaints++;
			ui_repaint_us += time_us() - u0;
		}
		ui_us += time_us() - u0;
		ui_frames++;
		stack_offset(&sx, &sy);
#ifdef SGX_BASE
		/* Set once, so the look only changes when the picture does. */
		if (!have_gpu || !stack_app_still()) {
			flatmesh_view(1.1f, (float)sx / fb.width * 0.05f, (float)sy / fb.height * 0.05f, 1.0f - stack_dim());
			mesh_colors(stack_launcher());
		}
#else
		flatmesh_view(1.1f, (float)sx / fb.width * 0.05f, (float)sy / fb.height * 0.05f, 1.0f - stack_dim());
		mesh_colors(stack_launcher());
#endif
		one_mesh = have_gpu && stack_app_still();
		if (one_mesh) {
			flatmesh_view(1.1f, 0.0f, 0.0f, 1.0f);
			flatmesh_colors(app_center, app_outer);
			shown_center = ~0u;
		}
	}
	if (frame) {
		shown_step = step;
		bool untouched = false;
		if (have_gpu) {
			gpu_flatmesh(buf, step * MESH_STEP_MS, have_ui);
#ifdef SGX_BASE
			/* The render runs on while the display sends the frame before. */
			if (sgx_compose_overlap_on()) {
				uint32_t w0 = time_us();
				display_wait_idle();
				uint32_t w1 = time_us();
				if (!sgx_compose_settle()) {
					if (gpu_fail++ == 0)
						log_printf("gpu: draw did not complete\n");
					have_gpu = false;
				}
				flip_us += w1 - w0;
				end_us += time_us() - w1;
			}
#endif
			untouched = gpu_draw_untouched();
		} else {
			flatmesh_render(buf, step * MESH_STEP_MS);
			if (have_ui) {
				ui_render(buf, fb.stride, 0, 0);
				rotation_buffer(buf, fb.stride);
			}
		}
		if (!have_ui) {
			draw_dot(buf);
			fbcon_draw(buf);
			untouched = false;
		}
		uint32_t f0 = time_us();
		if (untouched)
			display_flip_untouched();
		else
			display_flip();
		flip_us += time_us() - f0;
		/* The indicators pop out once the first frame is on the panel. */
		if (woke && have_ui)
			stack_indicators();
		woke = false;
	} else {
		/* Masked, so an event posted just before cannot wait a tick. */
		uint32_t w0 = time_us();
		uint32_t flags = irq_save();
		if (!sched_pending())
			wfi();
		irq_restore(flags);
		waited_us = time_us() - w0;
		power_count_idle(waited_us);
	}
#ifdef SGX_BASE
	sgx_compose_settle();
	damage_later();
	paint_wait();
#endif
}

uint32_t frame_waited_us(void) { return waited_us; }
uint32_t frame_rest_ms(void) { return 0; }

/* The last second's numbers, for the log. */
void frame_log(void)
{
#ifdef SGX_BASE
	if (gpu_frames)
		log_printf("gpu: %lu frames, %lu us submit to idle, %lu us vertices, %lu kept\n", gpu_frames,
			   gpu_us / gpu_frames, gpu_vert_us / gpu_frames, mesh_kept);
	if (bands_checked || bands_waits)
		log_printf("bands: %s, %lu waits, %lu checked, %lu outside\n",
			   bands_mode == FRAME_BANDS_VERIFY ? "verify" : bands_mode ? "painted" : "scan", bands_waits,
			   bands_checked, bands_outside);
	mesh_kept = bands_checked = bands_outside = bands_waits = 0;
#else
	if (gpu_frames)
		log_printf("gpu: %lu frames, %lu us submit to idle, %lu us vertices\n", gpu_frames,
			   gpu_us / gpu_frames, gpu_vert_us / gpu_frames);
#endif
	if (gpu_frames)
		log_printf("frame: mesh %lu, layers %lu, end %lu, flip %lu us\n", mesh_us / gpu_frames, layer_us / gpu_frames,
			   end_us / gpu_frames, flip_us / gpu_frames);
	gpu_frames = gpu_us = gpu_vert_us = mesh_us = layer_us = end_us = flip_us = 0;
	if (ui_frames)
		log_printf("ui: %lu of %lu frames repainted, update %lu us avg, %lu us per repaint, idle %lu%%\n", ui_repaints,
			   ui_frames, ui_us / ui_frames, ui_repaints ? ui_repaint_us / ui_repaints : 0,
			   idle_passes * 100 / ui_frames);
	ui_frames = ui_us = ui_repaints = ui_repaint_us = idle_passes = 0;
#ifdef SGX_BASE
	paint_log();
#endif
}
