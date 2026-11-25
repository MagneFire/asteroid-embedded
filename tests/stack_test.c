/* The screen stack on the host: swipes between the screens, drags
 * that belong to a scrolling screen, an app opening, going back and
 * closing, the app kept across a short sleep, the strip of app pages,
 * and the band scan that finds a layer's content. */
#include <stdio.h>
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "gpu/draw.h"
#include "ui/ui.h"
#include "ui/stack.h"
#include "stubs.h"

#define W 400
#define H 400

static uint32_t layers[UI_PANELS][W * H];
static int touches[UI_PANELS], last_kind = -1, last_panel = -1, last_x;
static unsigned screen_set = UI_PANELS;
static int app_set = 1;
static struct gpu_rect face_parts[32];
static unsigned face_n;
static int face_x, face_y;

void ui_touch(uint32_t panel, int kind, int x, int y)
{
	touches[panel]++;
	last_kind = kind;
	last_panel = (int)panel;
	last_x = x;
}
void ui_set_screen(unsigned panel) { screen_set = panel; }
static bool modal;
bool ui_modal(void) { return modal; }
void ui_set_app(int app) { app_set = app; }
static bool no_launcher;
bool ui_has_panel(unsigned panel) { return !(no_launcher && panel == UI_PANEL_BOTTOM); }
/* Both lists at their start: a swipe back is a swipe, not a scroll. */
unsigned ui_notes_edges(void) { return 16 | 0xf; }
static unsigned launcher_edges = 16 | 0xf;
unsigned ui_launcher_edges(void) { return launcher_edges; }
void gpu_draw_layer_parts(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha,
			  const struct gpu_rect *parts, unsigned n)
{
	if (tex_addr != (uint32_t)(uintptr_t)layers[UI_PANEL_FACE])
		return;
	face_n = n < 32 ? n : 32;
	memcpy(face_parts, parts, face_n * sizeof parts[0]);
	face_x = x;
	face_y = y;
}
static int round_draws, round_x, round_y;
void gpu_draw_layer_round(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha)
{
	round_draws++;
	round_x = x;
	round_y = y;
}
static uint32_t pool[4][32 * 32];
static unsigned pool_n;
void *gpu_alloc(uint32_t size, uint32_t align) { return pool_n < 4 ? pool[pool_n++] : NULL; }
static int bottom_y;
void gpu_draw_layer_at(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha)
{
	if (x == W / 2 - (int)width / 2 && y > H / 2)
		bottom_y = y + (int)height / 2;
}
/* The strip: Timer, Music and the Stopwatch, which takes sideways drags. */
static const int page_apps[] = { 3, 0, 4 };
static unsigned page_count;
int ui_page(unsigned i, bool *sideways)
{
	if (i >= page_count)
		return -1;
	if (sideways)
		*sideways = page_apps[i] == 4;
	return page_apps[i];
}
int ui_app_open(void) { return app_set; }
static void open_page(int app)
{
	ui_set_app(app);
	stack_open_app();
}
static int depth, peek_px = -1, peek_ended = -1;
int ui_app_depth(void) { return depth; }
void ui_app_peek(int px) { peek_px = px; }
void ui_app_peek_end(bool back)
{
	peek_ended = back;
	peek_px = 0;
}
static int app_x = -1;
static void background(int x, int y, float alpha) { app_x = x; }

static void touch(int kind, int x, int y)
{
	stack_touch(kind == UI_TOUCH_DOWN ? UI_TOUCH_DOWN : kind, x, y, stub_ms);
}

/* A quick straight drag of n moves, from (x0, y0) to (x1, y1). */
static void drag(int x0, int y0, int x1, int y1, int n)
{
	touch(UI_TOUCH_DOWN, x0, y0);
	for (int i = 1; i <= n; i++) {
		stub_ms += 10;
		touch(UI_TOUCH_MOVE, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n);
	}
	stub_ms += 10;
	touch(UI_TOUCH_UP, x1, y1);
}

static void settle(void)
{
	for (int i = 0; i < 40; i++) {
		stub_ms += 10;
		stack_update(stub_ms);
	}
}

static void test_swipes(void)
{
	stub_ms = 1000;
	fb.width = W;
	fb.height = H;
	fb.stride = W;
	stack_init(W, H, layers[0], W * H);
	stack_update(stub_ms);
	CHECK(stack_screen() == UI_PANEL_FACE && screen_set == UI_PANEL_FACE);

	/* Right: the notifications. The press is cancelled on the face. */
	touches[UI_PANEL_FACE] = 0;
	drag(50, 200, 250, 200, 4);
	CHECK(stack_screen() == UI_PANEL_LEFT && stack_moving());
	CHECK(last_panel == UI_PANEL_FACE && last_kind == UI_TOUCH_UP);
	settle();
	int x, y;
	stack_offset(&x, &y);
	CHECK(!stack_moving() && x == W && y == 0 && screen_set == UI_PANEL_LEFT);
	CHECK(stack_dim() > 0.0f && stack_launcher() == 0.0f);
	/* Back with the opposite swipe. */
	drag(350, 200, 150, 200, 4);
	settle();
	CHECK(stack_screen() == UI_PANEL_FACE);
	/* A vertical drag on the notifications scrolls them. */
	drag(50, 200, 250, 200, 4);
	settle();
	touches[UI_PANEL_LEFT] = 0;
	drag(200, 100, 200, 300, 4);
	CHECK(stack_screen() == UI_PANEL_LEFT && touches[UI_PANEL_LEFT] == 6 && last_kind == UI_TOUCH_UP);
	settle();
	drag(350, 200, 150, 200, 4);
	settle();
	CHECK(stack_screen() == UI_PANEL_FACE);

	/* Up: the launcher; a drag down on the horizontal layout goes back. */
	stack_set_launcher(0);
	drag(200, 350, 200, 150, 4);
	settle();
	stack_offset(&x, &y);
	CHECK(stack_screen() == UI_PANEL_BOTTOM && x == 0 && y == -H);
	/* Towards the launcher the mesh takes its colours instead of dimming. */
	CHECK(stack_launcher() == 1.0f && stack_dim() == 0.0f);
	drag(200, 100, 200, 300, 4);
	settle();
	CHECK(stack_screen() == UI_PANEL_FACE && stack_dim() == 0.0f);
	/* The speed dial keeps a drag on its wheel, one on its name goes back. */
	stack_set_launcher(6);
	drag(200, 350, 200, 150, 4);
	settle();
	CHECK(stack_screen() == UI_PANEL_BOTTOM);
	drag(80, 150, 80, 350, 4);
	settle();
	CHECK(stack_screen() == UI_PANEL_BOTTOM);
	launcher_edges = 4 | 32;
	drag(80, 150, 80, 350, 4);
	settle();
	launcher_edges = 16 | 0xf;
	CHECK(stack_screen() == UI_PANEL_FACE);
	stack_set_launcher(0);
	/* A short slow drag stays. */
	drag(200, 200, 215, 200, 8);
	settle();
	CHECK(stack_screen() == UI_PANEL_FACE);
	/* Down and left. */
	drag(200, 50, 200, 250, 4);
	settle();
	CHECK(stack_screen() == UI_PANEL_TOP);
	drag(200, 350, 200, 150, 4);
	settle();
	drag(350, 200, 150, 200, 4);
	settle();
	CHECK(stack_screen() == UI_PANEL_RIGHT);
	/* A vertical drag on Today scrolls its events. */
	touches[UI_PANEL_RIGHT] = 0;
	drag(200, 300, 200, 100, 4);
	CHECK(stack_screen() == UI_PANEL_RIGHT && touches[UI_PANEL_RIGHT] == 6 && last_kind == UI_TOUCH_UP);
	settle();
	drag(50, 200, 250, 200, 4);
	settle();
	CHECK(stack_screen() == UI_PANEL_FACE);
}

static void test_app(void)
{
	/* An app opened after the pass read its time still slides in. */
	uint32_t pass = stub_ms;
	stub_ms += 2;
	stack_open_app();
	stack_update(pass);
	stack_compose(background);
	CHECK(app_x == W);
	stub_ms += 50;
	stack_update(stub_ms);
	stack_compose(background);
	CHECK(app_x > 0 && app_x < W);
	CHECK(stack_moving());
	settle();
	CHECK(!stack_moving());
	/* A drag off the edge is the app's. */
	touches[UI_PANEL_APP] = 0;
	drag(200, 200, 300, 200, 4);
	CHECK(touches[UI_PANEL_APP] == 6);
	/* A drag from the left edge, far enough, closes it onto the launcher. */
	app_set = 1;
	drag(10, 200, 300, 200, 4);
	settle();
	CHECK(app_set == -1);
	CHECK(stack_screen() == UI_PANEL_BOTTOM);
	/* One from the top edge closes it onto the watchface. */
	stack_open_app();
	settle();
	app_set = 1;
	drag(200, 10, 200, 300, 4);
	settle();
	CHECK(app_set == -1);
	CHECK(stack_screen() == UI_PANEL_FACE);
	/* A short one snaps back. */
	stack_open_app();
	settle();
	app_set = 1;
	drag(10, 200, 50, 200, 4);
	settle();
	CHECK(app_set == 1);
	/* A page back let go of after the pass read its time still eases
	 * the rest of the way, rather than jumping there. */
	depth = 1;
	touch(UI_TOUCH_DOWN, 10, 200);
	for (int i = 1; i <= 4; i++) {
		stub_ms += 10;
		touch(UI_TOUCH_MOVE, 10 + 30 * i, 200);
	}
	CHECK(peek_px == 120);
	pass = stub_ms;
	stub_ms += 2;
	touch(UI_TOUCH_UP, 130, 200);
	stack_update(pass);
	CHECK(peek_px == 120 && peek_ended == -1);
	stub_ms += 50;
	stack_update(stub_ms);
	CHECK(peek_px > 120 && peek_px < W);
	settle();
	CHECK(peek_ended == 1);
	depth = 0;
	/* A pop-up during a press lets go of it outside the button, and the
	 * release does not reach the app, also once the pop-up is gone. */
	touches[UI_PANEL_APP] = touches[UI_PANEL_OVERLAY] = 0;
	touch(UI_TOUCH_DOWN, 200, 200);
	modal = true;
	touch(UI_TOUCH_MOVE, 201, 200);
	CHECK(touches[UI_PANEL_APP] == 2 && last_kind == UI_TOUCH_UP && last_x == -10000);
	touch(UI_TOUCH_UP, 201, 200);
	modal = false;
	touch(UI_TOUCH_DOWN, 200, 200);
	modal = true;
	touch(UI_TOUCH_MOVE, 201, 200);
	modal = false;
	touch(UI_TOUCH_UP, 201, 200);
	CHECK(touches[UI_PANEL_APP] == 4 && touches[UI_PANEL_OVERLAY] == 0 && last_x == -10000);
	/* So does a sleep and quick wake. */
	touch(UI_TOUCH_DOWN, 200, 200);
	stack_sleep(stub_ms);
	CHECK(touches[UI_PANEL_APP] == 6 && last_kind == UI_TOUCH_UP && last_x == -10000);
	stack_wake(stub_ms + 100);
	touch(UI_TOUCH_UP, 200, 200);
	CHECK(app_set == 1 && touches[UI_PANEL_APP] == 6);
	settle();
	/* Kept across a short sleep, closed after a long one. */
	stack_sleep(stub_ms);
	stub_ms += 4999;
	stack_wake(stub_ms);
	CHECK(app_set == 1);
	stack_sleep(stub_ms);
	stub_ms += 5000;
	stack_wake(stub_ms);
	CHECK(app_set == -1);
	stack_home();
	CHECK(stack_screen() == UI_PANEL_FACE);
}

/* A build without the launcher (the Cortex-M one): a swipe up stays on
 * the watchface without moving it, the others still go, and an app sent
 * back from its left edge closes onto the watchface. */
static void test_no_launcher(void)
{
	no_launcher = true;
	stack_init(W, H, layers[0], W * H);
	stack_update(stub_ms);
	touch(UI_TOUCH_DOWN, 200, 350);
	for (int i = 1; i <= 4; i++) {
		stub_ms += 10;
		touch(UI_TOUCH_MOVE, 200, 350 - 50 * i);
		int x, y;
		stack_offset(&x, &y);
		CHECK(x == 0 && y == 0);
	}
	stub_ms += 10;
	touch(UI_TOUCH_UP, 200, 150);
	settle();
	CHECK(stack_screen() == UI_PANEL_FACE && screen_set == UI_PANEL_FACE);
	drag(200, 50, 200, 250, 4);
	settle();
	CHECK(stack_screen() == UI_PANEL_TOP);
	drag(200, 350, 200, 150, 4);
	settle();
	CHECK(stack_screen() == UI_PANEL_FACE);
	stack_open_app();
	settle();
	app_set = 1;
	drag(10, 200, 300, 200, 4);
	settle();
	CHECK(app_set == -1 && stack_screen() == UI_PANEL_FACE);
	no_launcher = false;
	stack_init(W, H, layers[0], W * H);
}

/* Without the launcher, the app pages: up from the watchface opens the
 * page last shown, a sideways drag turns to the next or previous one on
 * release, wrapping round, an app with its own sideways drags keeps
 * those in its middle, and a short slow drag stays. */
static void test_pages(void)
{
	unsigned n;
	int next;
	no_launcher = true;
	page_count = 3;
	stack_init(W, H, layers[0], W * H);
	stack_set_pages(open_page);
	stack_update(stub_ms);
	app_set = -1;
	drag(200, 350, 200, 150, 4);
	settle();
	CHECK(app_set == 3 && stack_panel() == UI_PANEL_APP);
	CHECK(stack_page(&n, &next) == 0 && n == 3 && next == -1 && stack_edges() == 4);
	touch(UI_TOUCH_DOWN, 300, 200);
	for (int i = 1; i <= 4; i++) {
		stub_ms += 10;
		touch(UI_TOUCH_MOVE, 300 - 40 * i, 200);
	}
	CHECK(app_set == 3 && stack_page(&n, &next) == 0 && next == 1);
	CHECK(last_panel == UI_PANEL_APP && last_kind == UI_TOUCH_UP);
	stub_ms += 10;
	touch(UI_TOUCH_UP, 140, 200);
	CHECK(app_set == 0 && stack_page(&n, &next) == 1 && next == -1);
	settle();
	/* A pop-up that comes and goes during a turn ends it. */
	touch(UI_TOUCH_DOWN, 300, 200);
	for (int i = 1; i <= 4; i++) {
		stub_ms += 10;
		touch(UI_TOUCH_MOVE, 300 - 40 * i, 200);
	}
	modal = true;
	touch(UI_TOUCH_MOVE, 130, 200);
	modal = false;
	CHECK(stack_page(&n, &next) == 1 && next == -1);
	touch(UI_TOUCH_MOVE, 120, 200);
	touch(UI_TOUCH_UP, 120, 200);
	CHECK(app_set == 0 && stack_page(&n, &next) == 1);
	drag(10, 200, 250, 200, 4);
	CHECK(app_set == 3);
	drag(100, 200, 300, 200, 4);
	CHECK(app_set == 4 && stack_page(&n, &next) == 2);
	settle();
	touches[UI_PANEL_APP] = 0;
	drag(300, 200, 100, 200, 4);
	CHECK(app_set == 4 && touches[UI_PANEL_APP] == 6);
	drag(395, 200, 150, 200, 4);
	CHECK(app_set == 3);
	drag(200, 200, 150, 200, 10);
	CHECK(app_set == 3);
	/* A short flick turns, a tap that slips a little stays the app's. */
	drag(200, 200, 168, 200, 2);
	CHECK(app_set == 0);
	drag(200, 200, 232, 200, 2);
	CHECK(app_set == 3);
	touches[UI_PANEL_APP] = 0;
	drag(200, 200, 191, 200, 1);
	CHECK(app_set == 3 && touches[UI_PANEL_APP] == 3 && last_kind == UI_TOUCH_UP);
	drag(300, 200, 100, 200, 4);
	CHECK(app_set == 0);
	settle();
	drag(200, 10, 200, 300, 4);
	settle();
	CHECK(app_set == -1 && stack_panel() == UI_PANEL_FACE && stack_edges() == 0xf);
	drag(200, 350, 200, 150, 4);
	settle();
	CHECK(app_set == 0);
	/* A swipe while it closes does not open another page. */
	drag(200, 10, 200, 300, 4);
	drag(300, 200, 100, 200, 4);
	settle();
	CHECK(app_set == -1 && stack_panel() == UI_PANEL_FACE);
	stack_home();
	page_count = 0;
	no_launcher = false;
	stack_init(W, H, layers[0], W * H);
	stack_set_pages(open_page);
}

static void test_damage(void)
{
	uint32_t *face = layers[UI_PANEL_FACE];
	memset(face, 0, sizeof layers[0]);
	for (int r = 0; r < 16; r++)
		face[r * W + 100] = face[r * W + 300] = 0xff0000ff;
	for (int r = 16; r < 32; r++)
		face[r * W + 150] = 0xff0000ff;
	stack_damage(UI_PANEL_FACE, 0, 0, W, 32);
	stack_update(stub_ms);
	stack_compose(background);
	CHECK(face_x == 0 && face_y == 0 && face_n == 2);
	CHECK(face_parts[0].x == 100 && face_parts[0].y == 0 && face_parts[0].width == 201 && face_parts[0].height == 16);
	CHECK(face_parts[1].x == 150 && face_parts[1].y == 16 && face_parts[1].width == 1 && face_parts[1].height == 16);
	/* A partial repaint widens a band, a whole one replaces it. */
	face[5 * W + 20] = 0xff0000ff;
	stack_damage(UI_PANEL_FACE, 0, 0, 64, 16);
	stack_compose(background);
	CHECK(face_n == 2 && face_parts[0].x == 20 && face_parts[0].width == 281);
	memset(face, 0, 16 * W * 4);
	face[8 * W + 200] = 0xff0000ff;
	stack_damage(UI_PANEL_FACE, 0, 0, W, 16);
	stack_compose(background);
	CHECK(face_n == 2 && face_parts[0].x == 200 && face_parts[0].width == 1);
	/* Bands with the same columns merge into one rectangle. */
	memset(face, 0, sizeof layers[0]);
	for (int r = 0; r < 64; r++)
		face[r * W + 10] = 0xff0000ff;
	stack_damage(UI_PANEL_FACE, 0, 0, W, H);
	stack_compose(background);
	CHECK(face_n == 1 && face_parts[0].x == 10 && face_parts[0].y == 0 && face_parts[0].width == 1 && face_parts[0].height == 64);
}

/* Dragged from the top and shown, then let go; it closes. */
static void drag_app_shown(void)
{
	stack_open_app();
	settle();
	touch(UI_TOUCH_DOWN, 200, 10);
	stub_ms += 10;
	touch(UI_TOUCH_MOVE, 200, 110);
	stack_compose(background);
	stub_ms += 10;
	touch(UI_TOUCH_UP, 200, 110);
	settle();
}

static void test_app_round(void)
{
	/* A round panel cuts the app only while it is moved. */
	stack_set_round(true);
	stack_open_app();
	settle();
	round_draws = 0;
	stack_compose(background);
	CHECK(round_draws == 0);
	stack_home();
	drag_app_shown();
	CHECK(round_draws == 1 && round_x == 0 && round_y == 100);
	/* A square one never does. */
	stack_set_round(false);
	round_draws = 0;
	drag_app_shown();
	CHECK(round_draws == 0);
	stack_home();
}

static void test_chin(void)
{
	/* The bottom indicator sits on the last line the panel shows. */
	stack_home();
	stub_ms += 1000;
	stack_compose(background);
	CHECK(bottom_y == H);
	stack_set_chin(30);
	stack_compose(background);
	CHECK(bottom_y == H - 30);
	stack_set_chin(0);
}

int main(void)
{
	test_swipes();
	test_app();
	test_no_launcher();
	test_pages();
	test_app_round();
	test_damage();
	test_chin();
	printf("stack_test: %s\n", stub_failures ? "FAILED" : "ok");
	return stub_failures != 0;
}
