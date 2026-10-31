/* The screen stack on the host: swipes between the screens, drags
 * that belong to a scrolling screen, an app opening, going back and
 * closing, the app kept across a short sleep, and the band scan that
 * finds a layer's content. */
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
static int touches[UI_PANELS], last_kind = -1, last_panel = -1;
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
}
void ui_set_screen(unsigned panel) { screen_set = panel; }
void ui_set_app(int app) { app_set = app; }
/* Both lists at their start: a swipe back is a swipe, not a scroll. */
unsigned ui_notes_edges(void) { return 16 | 0xf; }
unsigned ui_launcher_edges(void) { return 16 | 0xf; }
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
static void background(int x, int y, float alpha) {}

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
	drag(50, 200, 250, 200, 4);
	settle();
	CHECK(stack_screen() == UI_PANEL_FACE);
}

static void test_app(void)
{
	stack_open_app();
	CHECK(stack_moving());
	settle();
	CHECK(!stack_moving());
	/* A drag off the edge is the app's. */
	touches[UI_PANEL_APP] = 0;
	drag(200, 200, 300, 200, 4);
	CHECK(touches[UI_PANEL_APP] == 6);
	/* A drag from the left edge, far enough, closes it. */
	app_set = 1;
	drag(10, 200, 300, 200, 4);
	settle();
	CHECK(app_set == -1);
	/* A short one snaps back. */
	stack_open_app();
	settle();
	app_set = 1;
	drag(10, 200, 50, 200, 4);
	settle();
	CHECK(app_set == 1);
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

int main(void)
{
	test_swipes();
	test_app();
	test_damage();
	printf("stack_test: %s\n", stub_failures ? "FAILED" : "ok");
	return stub_failures != 0;
}
