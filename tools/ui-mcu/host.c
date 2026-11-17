/* The Cortex-M UI off the watch: draws each screen the way the Apollo3
 * firmware would, into a frame of RGB565 pixels, and reports the heap's
 * peak after each. A heap too small ends it: exit status 2 under emu.py,
 * an abort on the host.
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

#include "../../src/ui/ui.h"

#ifndef W
#define W 402
#define H 476
#endif

static uint16_t frame[H][W];
static uint32_t now_ms;
static uint32_t box[4];

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
#else
static void fail(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(3);
}
#endif

uint32_t time_ms(void)
{
	return now_ms;
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
		ui_draw(panel, false);
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

static void shot(uint32_t panel, const char *name)
{
#ifdef EMU
	emu_screen(name);
#endif
	ui_set_screen(panel == UI_PANEL_APP ? UI_PANEL_FACE : panel);
	run(panel, 1500);
#ifdef EMU
	emu_shot(name, &frame[0][0], ui_heap_peak());
#else
	png(out_dir, name);
	printf("%-16s heap peak %6u\n", name, ui_heap_peak());
	fflush(stdout);
#endif
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
	shot(UI_PANEL_FACE, "aod");
	ui_set_ambient(false);
	ui_set_face(1, false, false);
	shot(UI_PANEL_FACE, "classic");
	ui_set_face(0, false, false);
	ui_show_notification(1);
	shot(UI_PANEL_FACE, "popup");
	ui_hide_notification();
	shot(UI_PANEL_LEFT, "notes");
	shot(UI_PANEL_RIGHT, "today");
	shot(UI_PANEL_TOP, "quick");
	for (int l = 0; l < UI_LAUNCHERS; l++) {
		char name[] = "launcher0";
		name[8] += l;
		ui_set_launcher(l);
		shot(UI_PANEL_BOTTOM, name);
	}
	ui_set_launcher(0);
	static const char *const app_names[] = { "music", "settings", "alarm", "timer", "stopwatch",
						 "weather", "heart", "flashlight", "calculator", "diamonds" };
	for (int a = 0; a < 10; a++) {
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
	shot(UI_PANEL_LEFT, "notes-full");
	ui_show_notification(20);
	shot(UI_PANEL_FACE, "popup-long");
	ui_hide_notification();
	ui_set_ringing(true, false);
	shot(UI_PANEL_FACE, "ringing");
	ui_set_ringing(false, false);
	shot(UI_PANEL_FACE, "face-again");
	return 0;
}

#ifdef EMU
void reset(void)
{
	emu_exit(main(0, NULL), "", 0);
}
#endif
