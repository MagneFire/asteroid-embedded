/* The Cortex-M UI off the watch: draws each screen the way the Apollo3
 * firmware would, into a frame of RGB565 pixels, and reports the heap's
 * peak after each. A heap too small ends it: exit status 2 under emu.py,
 * an abort on the host. Then it walks the screens with the firmware's
 * screen stack, swiping each way from the watchface and tapping across
 * each screen it reaches; a blank screen, a screen or an app the build
 * left out, or a panic fails it.
 *
 * On the host, with a host build of the library (run-host.sh), the frames
 * are written out as PNG files.
 * usage: host HEAP_BYTES OUT_DIR
 *
 * Built with -DEMU for the Cortex-M4 with the firmware's build of the
 * library, it runs in emu.py, which sets heap_size, and is told of each
 * screen as it starts, each frame and the exit through emu_screen,
 * emu_shot and emu_exit.
 *
 * -DW=... -DH=... draw another screen size than the 46 mm watch's. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifndef EMU
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#endif

#include "../../src/display/display.h"
#include "../../src/gpu/draw.h"
#include "../../src/ui/ui.h"
#include "../../src/ui/stack.h"
#include "../../src/display/flatmesh_mcu.h"

#ifndef W
#define W 402
#define H 476
#endif

static uint16_t frame[H][W];
static uint32_t now_ms;
static uint32_t box[4];
/* What frame_mcu.c would draw under the UI. */
static struct backdrop backdrop;

#ifdef EMU
volatile uint32_t heap_size;
static uint8_t heap[256 * 1024] __attribute__((aligned(8)));

__attribute__((noinline)) void emu_shot(const char *name, const uint16_t *pixels, uint32_t peak)
{
	__asm__ volatile("" ::"r"(name), "r"(pixels), "r"(peak) : "memory");
}

__attribute__((noinline)) void emu_screen(const char *name)
{
	__asm__ volatile("" ::"r"(name) : "memory");
}

__attribute__((noinline, noreturn)) void emu_exit(int status, const char *msg, size_t len)
{
	__asm__ volatile("" ::"r"(status), "r"(msg), "r"(len) : "memory");
	for (;;)
		;
}

static size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

static void fail(const char *msg)
{
	emu_exit(3, msg, strlen(msg));
}

/* emu.py prints the name as a screen's. */
static void fail_on(const char *why, const char *name)
{
	emu_shot(name, NULL, 0);
	fail(why);
}
#else
static void fail(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(3);
}

static void fail_on(const char *why, const char *name)
{
	fprintf(stderr, "%s: %s\n", name, why);
	exit(3);
}
#endif

uint32_t time_ms(void)
{
	return now_ms;
}

/* The stack draws no layers here: frame_mcu.c's way, ui_draw. */
struct fb fb;
void *gpu_alloc(uint32_t size, uint32_t align) { return NULL; }
void gpu_draw_layer_at(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha) {}
void gpu_draw_layer_round(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha) {}
void gpu_draw_layer_parts(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha,
			  const struct gpu_rect *parts, unsigned n)
{
}

void ui_panic(const char *msg, size_t len)
{
#ifdef EMU
	emu_exit(2, msg, len);
#else
	fprintf(stderr, "ui panic: %.*s\n", (int)len, msg);
	exit(2);
#endif
}

void ui_draw_begin(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	box[0] = x, box[1] = y, box[2] = w, box[3] = h;
	if (x + w > W || y + h > H)
		fail("draw box off the screen");
}

void ui_draw_line(uint32_t y, uint32_t x, uint32_t w, const uint16_t *rgb565)
{
	if (y < box[1] || y >= box[1] + box[3] || x < box[0] || x + w > box[0] + box[2])
		fail("line outside the draw box");
	for (uint32_t i = 0; i < w; i++)
		frame[y][x + i] = rgb565[i];
}

void ui_draw_end(void)
{
}

#ifndef EMU
static void put32(FILE *f, uint32_t v)
{
	uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v };
	fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
	put32(f, len);
	fwrite(type, 1, 4, f);
	fwrite(data, 1, len, f);
	uint32_t crc = crc32(crc32(0, (const uint8_t *)type, 4), data, len);
	put32(f, crc);
}

static void png(const char *dir, const char *name)
{
	static uint8_t raw[H * (1 + W * 3)], packed[H * (1 + W * 3) + 1024];
	for (int y = 0; y < H; y++) {
		uint8_t *row = &raw[y * (1 + W * 3)];
		row[0] = 0;
		for (int x = 0; x < W; x++) {
			uint16_t p = frame[y][x];
			row[1 + x * 3] = (p >> 11) * 255 / 31;
			row[2 + x * 3] = (p >> 5 & 63) * 255 / 63;
			row[3 + x * 3] = (p & 31) * 255 / 31;
		}
	}
	uLongf n = sizeof(packed);
	compress(packed, &n, raw, sizeof(raw));
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.png", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		exit(1);
	}
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	uint8_t ihdr[13] = { 0, 0, W >> 8, W & 255, 0, 0, H >> 8, H & 255, 8, 2, 0, 0, 0 };
	chunk(f, "IHDR", ihdr, 13);
	chunk(f, "IDAT", packed, n);
	chunk(f, "IEND", NULL, 0);
	fclose(f);
}
#endif

/* Lets animations and timers run for ms, drawing the panel every 16 ms as
 * the firmware would. */
static void run(uint32_t panel, uint32_t ms)
{
	for (uint32_t t = 0; t <= ms; t += 16) {
		now_ms += 16;
		ui_tick();
		flatmesh_mcu_draw(ui_modal() ? UI_PANEL_OVERLAY : panel, &backdrop, false);
	}
}

static void touch(uint32_t panel, int kind, int x, int y)
{
	ui_set_touch_ms(now_ms);
	ui_touch(panel, kind, x, y);
}

static void tap(uint32_t panel, int x, int y)
{
	touch(panel, UI_TOUCH_DOWN, x, y);
	run(panel, 50);
	touch(panel, UI_TOUCH_UP, x, y);
	run(panel, 600);
}

#ifndef EMU
static const char *out_dir;
#endif

/* Draws all of panel afresh into the frame: false if it is all black. */
static bool drawn(uint32_t panel)
{
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			frame[y][x] = 0;
	ui_draw(panel, true);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			if (frame[y][x])
				return true;
	return false;
}

static void save(const char *name)
{
#ifdef EMU
	emu_shot(name, &frame[0][0], ui_heap_peak());
#else
	png(out_dir, name);
	printf("%-16s heap peak %6u\n", name, ui_heap_peak());
	fflush(stdout);
#endif
}

static void shot(uint32_t panel, const char *name)
{
#ifdef EMU
	emu_screen(name);
#endif
	ui_set_screen(panel == UI_PANEL_APP ? UI_PANEL_FACE : panel);
	backdrop.screen = panel;
	run(panel, 1500);
	if (!drawn(panel))
		fail_on("blank", name);
	save(name);
}

/* The firmware's pass (frame_mcu.c), with the events the UI sent: none
 * may open an app the build left out. */
static void pass(uint32_t ms)
{
	for (uint32_t t = 0; t <= ms; t += 16) {
		now_ms += 16;
		stack_update(now_ms);
		ui_tick();
		ui_draw(ui_modal() ? UI_PANEL_OVERLAY : stack_screen(), false);
		for (struct ui_event e; ui_next_event(&e);)
			if ((e.kind == UI_EVENT_APP && !ui_has_app(e.value)) ||
			    (e.kind == UI_EVENT_SETTING && e.value == UI_OPEN_SETTINGS && !ui_has_app(UI_APP_SETTINGS)))
				fail("walk: opens an app the build left out");
	}
}

static void stack_at(int kind, int x, int y)
{
	ui_set_touch_ms(now_ms);
	stack_touch(kind, x, y, now_ms);
}

static void swipe(int x0, int y0, int x1, int y1)
{
	stack_at(UI_TOUCH_DOWN, x0, y0);
	pass(16);
	for (int i = 1; i <= 6; i++) {
		stack_at(UI_TOUCH_MOVE, x0 + (x1 - x0) * i / 6, y0 + (y1 - y0) * i / 6);
		pass(16);
	}
	stack_at(UI_TOUCH_UP, x1, y1);
	pass(400);
}

/* What the stack shows must be in the build and drawn. */
static void check(const char *name)
{
	unsigned panel = ui_modal() ? UI_PANEL_OVERLAY : stack_screen();
	if (!ui_has_panel(panel))
		fail_on("a screen the build left out", name);
	if (!drawn(panel))
		fail_on("blank", name);
	save(name);
}

/* From the watchface each way (the finger's direction) and, where that
 * leads somewhere, taps and a long press across the screen, then back. */
static void walk(void)
{
	static const struct {
		const char *name;
		int dx, dy;
	} ways[] = { { "walk-right", 1, 0 }, { "walk-left", -1, 0 }, { "walk-down", 0, 1 }, { "walk-up", 0, -1 } };
	stack_init(W, H, NULL, 0);
	pass(100);
	check("walk-face");
	for (int y = 1; y < 5; y++)
		for (int x = 1; x < 5; x++) {
			stack_at(UI_TOUCH_DOWN, W * x / 5, H * y / 5);
			pass(50);
			stack_at(UI_TOUCH_UP, W * x / 5, H * y / 5);
			pass(300);
		}
	check("walk-face-taps");
	for (unsigned i = 0; i < sizeof(ways) / sizeof(ways[0]); i++) {
		int cx = W / 2, cy = H / 2, ax = W * 3 / 10 * ways[i].dx, ay = H * 3 / 10 * ways[i].dy;
		swipe(cx - ax, cy - ay, cx + ax, cy + ay);
		check(ways[i].name);
		if (stack_screen() == UI_PANEL_FACE)
			continue;
		for (int y = 1; y < 5; y++)
			for (int x = 1; x < 5; x++) {
				stack_at(UI_TOUCH_DOWN, W * x / 5, H * y / 5);
				pass(x == y ? 1000 : 50);
				stack_at(UI_TOUCH_UP, W * x / 5, H * y / 5);
				pass(300);
				ui_hide_notification();
				pass(16);
				if (ui_app_open() >= 0 && !ui_has_app(ui_app_open()))
					fail("walk: an app the build left out is open");
				if (!drawn(ui_modal() ? UI_PANEL_OVERLAY : stack_screen()))
					fail_on("blank after a tap", ways[i].name);
			}
		/* The remorse timers of reboot and power off run out. */
		pass(4000);
		swipe(cx + ax, cy + ay, cx - ax, cy - ay);
		if (stack_screen() != UI_PANEL_FACE)
			fail("walk: no way back to the watchface");
		check("walk-back");
	}
	/* A pop-up, tapped and swiped away. */
	ui_show_notification(1);
	pass(400);
	check("walk-popup");
	stack_at(UI_TOUCH_DOWN, W / 2, H / 2);
	pass(50);
	stack_at(UI_TOUCH_UP, W / 2, H / 2);
	pass(400);
	swipe(W / 4, H / 2, W * 3 / 4, H / 2);
	if (ui_modal())
		fail("walk: the pop-up stays");
	check("walk-end");
}

int main(int argc, char **argv)
{
#ifdef EMU
	if (heap_size > sizeof(heap))
		fail("heap_size too large");
#else
	if (argc < 3) {
		fprintf(stderr, "usage: %s HEAP_BYTES OUT_DIR\n", argv[0]);
		return 1;
	}
	uint32_t heap_size = strtoul(argv[1], NULL, 0);
	out_dir = argv[2];
	void *heap = malloc(heap_size);
#endif
	if (!ui_init(heap, heap_size, NULL, 0, 0, W, H))
		fail("ui_init failed");
	flatmesh_mcu_init(W, H);
#ifdef EMU
	emu_shot("init", NULL, ui_heap_peak());
#else
	printf("%-16s heap peak %6u\n", "init", ui_heap_peak());
#endif
	static const char board[] = "beluga";
	ui_set_board(board, sizeof(board) - 1, true, false, false, true, true, false);
	ui_set_clock(10, 8, 30, 3, 12, 11, 2026, 1000000);
	ui_set_battery(76, false);
	ui_set_connected(true, true);
	ui_set_passkey(-1);
	struct ui_settings s = { .brightness = 80, .nightstand_brightness = 20, .nightstand_delay = 5, .burn_in = 0,
				 .bluetooth = true, .vibration = true, .aod = true, .tilt = 1, .tap = true };
	ui_set_settings(&s);
	ui_set_build("asteroid-embedded mcu");
	ui_set_info("12:34:56:78:9A:BC", 3600);
	ui_set_face(0, false, false);
	ui_set_launcher(0);
	ui_set_app(-1);
	static const char *const apps[] = { "Messages", "Mail", "Calendar" };
	static const char *const summaries[] = { "Alice", "Weekly report", "Stand-up" };
	static const char *const bodies[] = { "Lunch at noon? There's a new place near the station.",
					      "Numbers for week 45 are in, 12% above plan.", "In 10 minutes, room 3" };
	static const char *const icons[] = { "ios-chatbubbles", "ios-mail", "ios-calendar" };
	for (int i = 0; i < 3; i++)
		ui_add_notification(i + 1, apps[i], strlen(apps[i]), summaries[i], strlen(summaries[i]), bodies[i],
				    strlen(bodies[i]), icons[i], strlen(icons[i]), 1000000 - 600 * (i + 1));
	int32_t ids[] = { 800, 500, 801, 600, 211 }, lo[] = { 4, 3, 1, -2, 0 }, hi[] = { 11, 9, 7, 3, 5 };
	ui_set_weather("Amsterdam", 9, 5, ids, lo, hi);
	ui_set_media("Clair de lune", 13, "Debussy", 7, "Suite bergamasque", 17, true, 60);
	static const struct ui_alarm alarms[] = { { 7, 30, 0x3e, true }, { 9, 0, 0x41, false } };
	ui_set_alarms(alarms, 2);

	shot(UI_PANEL_FACE, "face");
	ui_set_ambient(true);
	backdrop.ambient = true;
	shot(UI_PANEL_FACE, "aod");
	/* The next minute, moved against burn-in as power.c would at High. */
	ui_set_clock(10, 9, 30, 3, 12, 11, 2026, 1000060);
	ui_set_shift(-31, 17);
	shot(UI_PANEL_FACE, "aod-minute");
	ui_set_shift(0, 0);
	ui_set_ambient(false);
	backdrop.ambient = false;
	ui_set_face(1, false, false);
	shot(UI_PANEL_FACE, "classic");
	ui_set_face(0, false, false);
	ui_show_notification(1);
	shot(UI_PANEL_FACE, "popup");
	ui_hide_notification();
	if (ui_has_panel(UI_PANEL_LEFT))
		shot(UI_PANEL_LEFT, "notes");
	if (ui_has_panel(UI_PANEL_RIGHT))
		shot(UI_PANEL_RIGHT, "today");
	if (ui_has_panel(UI_PANEL_TOP))
		shot(UI_PANEL_TOP, "quick");
	for (int l = 0; ui_has_panel(UI_PANEL_BOTTOM) && l < UI_LAUNCHERS; l++) {
		char name[] = "launcher0";
		name[8] += l;
		ui_set_launcher(l);
		shot(UI_PANEL_BOTTOM, name);
	}
	ui_set_launcher(0);
	static const char *const app_names[] = { "music", "settings", "alarm", "timer", "stopwatch",
						 "weather", "heart", "flashlight", "calculator", "diamonds" };
	for (int a = 0; a < 10; a++) {
		if (!ui_has_app(a))
			continue;
		ui_set_app(a);
		shot(UI_PANEL_APP, app_names[a]);
		if (a == UI_APP_SETTINGS) {
			tap(UI_PANEL_APP, W / 2, H / 2);
			shot(UI_PANEL_APP, "settings-page");
		}
	}
	ui_set_app(-1);
	/* The most the phone can leave: NOTIFY_MAX notifications, each as long
	 * as notify keeps them. */
	static char summary[95], body[383];
	for (size_t i = 0; i < sizeof(summary); i++)
		summary[i] = "Weekly report "[i % 14];
	for (size_t i = 0; i < sizeof(body); i++)
		body[i] = "Numbers for week 45 are in, 12% above plan. "[i % 44];
	for (int i = 3; i < 20; i++)
		ui_add_notification(i + 1, apps[1], strlen(apps[1]), summary, sizeof(summary), body, sizeof(body), icons[1],
				    strlen(icons[1]), 1000000 - 600 * (i + 1));
	if (ui_has_panel(UI_PANEL_LEFT))
		shot(UI_PANEL_LEFT, "notes-full");
	ui_show_notification(20);
	shot(UI_PANEL_FACE, "popup-long");
	ui_hide_notification();
	ui_set_ringing(true, false);
	shot(UI_PANEL_FACE, "ringing");
	ui_set_ringing(false, false);
	shot(UI_PANEL_FACE, "face-again");
	walk();
	return 0;
}

#ifdef EMU
void reset(void)
{
	emu_exit(main(0, NULL), "", 0);
}
#endif
