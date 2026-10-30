/* Screen stack geometry and gestures. The watchface's origin (fx, fy) is
 * the single state the layout follows: at rest it is (0, 0) on the
 * watchface, (W, 0) with the notifications on screen, (-W, 0) for the
 * weather and (0, -H) for the music; the side screens sit one screen
 * away from the watchface. A drag moves the origin with the finger,
 * clamped to the screens that exist in that direction, and a release
 * past 30 % or a quick flick switches screens; the slide back to rest is
 * an ease-out over SLIDE_MS. */
#include "asteroid.h"
#include "display/display.h"
#include "gpu/draw.h"
#include "ui/ui.h"
#include "ui/stack.h"

#define SWIPE_PX	40
#define FLICK_MS	300
#define SLIDE_MS	250
#define DIM		0.4f

enum { IDLE, PENDING, SCROLL, SWIPE };
enum { LEFT, RIGHT, UP, DOWN };

static const bool scrolls[UI_PANELS] = { [UI_PANEL_LEFT] = true };

static int W, H, fx, fy, from_x, from_y;
static unsigned screen, gesture;
static bool animating, dragging, horizontal;
static int down_x, down_y;
static uint32_t down_ms, anim_t0;

static int rest_x(unsigned s) { return s == UI_PANEL_LEFT ? W : s == UI_PANEL_RIGHT ? -W : 0; }
static int rest_y(unsigned s) { return s == UI_PANEL_BOTTOM ? -H : 0; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int absi(int v) { return v < 0 ? -v : v; }

void stack_init(uint32_t width, uint32_t height)
{
	W = width;
	H = height;
	stack_home();
}

void stack_home(void)
{
	screen = UI_PANEL_FACE;
	fx = fy = 0;
	animating = dragging = false;
	gesture = IDLE;
}

unsigned stack_screen(void) { return screen; }
bool stack_moving(void) { return animating || dragging; }
void stack_offset(int *x, int *y) { *x = fx; *y = fy; }

static void slide_to(unsigned s)
{
	screen = s;
	from_x = fx;
	from_y = fy;
	anim_t0 = time_ms();
	animating = fx != rest_x(s) || fy != rest_y(s);
	dragging = false;
}

static void swipe(int dir)
{
	unsigned s = screen;
	if (screen == UI_PANEL_FACE)
		s = dir == RIGHT ? UI_PANEL_LEFT : dir == LEFT ? UI_PANEL_RIGHT : dir == UP ? UI_PANEL_BOTTOM : UI_PANEL_FACE;
	else if ((screen == UI_PANEL_LEFT && dir == LEFT) || (screen == UI_PANEL_RIGHT && dir == RIGHT) ||
		 (screen == UI_PANEL_BOTTOM && dir == DOWN))
		s = UI_PANEL_FACE;
	slide_to(s);
}

static void drag(int dx, int dy)
{
	if (horizontal)
		dy = 0;
	else
		dx = 0;
	switch (screen) {
	case UI_PANEL_FACE:
		dx = clampi(dx, -W, W);
		dy = clampi(dy, -H, 0);
		break;
	case UI_PANEL_LEFT:
		dx = clampi(dx, -W, 0);
		dy = 0;
		break;
	case UI_PANEL_RIGHT:
		dx = clampi(dx, 0, W);
		dy = 0;
		break;
	default:
		dx = 0;
		dy = clampi(dy, 0, H);
	}
	fx = rest_x(screen) + dx;
	fy = rest_y(screen) + dy;
	animating = false;
	dragging = true;
}

static void release(bool flick)
{
	int x = fx - rest_x(screen), y = fy - rest_y(screen);
	if (absi(x) * 10 > W * 3 || absi(y) * 10 > H * 3 || (flick && (x || y)))
		swipe(absi(x) >= absi(y) ? (x > 0 ? RIGHT : LEFT) : (y < 0 ? UP : DOWN));
	else
		slide_to(screen);
}

static void cancel_press(void)
{
	ui_touch(screen, UI_TOUCH_UP, -10000, -10000);
}

void stack_touch(int kind, int x, int y, uint32_t now_ms)
{
	int dx = x - down_x, dy = y - down_y;
	bool far = dx * dx + dy * dy > SWIPE_PX * SWIPE_PX;
	if (ui_modal()) {
		if (kind == UI_TOUCH_DOWN) {
			gesture = PENDING;
			down_x = x;
			down_y = y;
		} else if (gesture == PENDING && far) {
			gesture = IDLE;
			ui_touch(UI_PANEL_OVERLAY, UI_TOUCH_UP, -10000, -10000);
			ui_hide_notification();
			return;
		}
		if (gesture == PENDING)
			ui_touch(UI_PANEL_OVERLAY, kind, x, y);
		if (kind == UI_TOUCH_UP)
			gesture = IDLE;
		return;
	}
	bool scroll = absi(dy) >= absi(dx) && scrolls[screen];
	bool flick = now_ms - down_ms < FLICK_MS;
	switch (kind) {
	case UI_TOUCH_DOWN:
		gesture = PENDING;
		down_x = x;
		down_y = y;
		down_ms = now_ms;
		ui_touch(screen, UI_TOUCH_DOWN, x, y);
		break;
	case UI_TOUCH_MOVE:
		if (gesture == PENDING && far && !scroll) {
			gesture = SWIPE;
			horizontal = absi(dx) >= absi(dy);
			cancel_press();
			drag(dx, dy);
		} else if (gesture == SWIPE) {
			drag(dx, dy);
		} else if (gesture == PENDING || gesture == SCROLL) {
			if (far)
				gesture = SCROLL;
			ui_touch(screen, UI_TOUCH_MOVE, x, y);
		}
		break;
	case UI_TOUCH_UP:
		if (gesture == SWIPE) {
			drag(dx, dy);
			release(flick);
		} else if (gesture == PENDING && far && !scroll) {
			cancel_press();
			horizontal = absi(dx) >= absi(dy);
			drag(dx, dy);
			release(true);
		} else if (gesture == PENDING || gesture == SCROLL) {
			ui_touch(screen, UI_TOUCH_UP, x, y);
		}
		gesture = IDLE;
		break;
	}
}

void stack_update(uint32_t now_ms)
{
	if (!animating)
		return;
	uint32_t t = now_ms - anim_t0;
	if (t >= SLIDE_MS) {
		fx = rest_x(screen);
		fy = rest_y(screen);
		animating = false;
		return;
	}
	float u = 1.0f - (float)t / SLIDE_MS, p = 1.0f - u * u * u;
	fx = from_x + (int)((rest_x(screen) - from_x) * p);
	fy = from_y + (int)((rest_y(screen) - from_y) * p);
}

static void layer(unsigned panel, int x, int y)
{
	if (x <= -W || x >= W || y <= -H || y >= H)
		return;
	gpu_draw_layer_at(UI_LAYER_BASE + panel * UI_LAYER_STRIDE, W, H, fb.stride * 4, x, y, 1.0f);
}

float stack_dim(void)
{
	float ax = (float)absi(fx) / W, ay = (float)absi(fy) / H;
	return (ax > ay ? ax : ay) * DIM;
}

void stack_compose(void)
{
	layer(UI_PANEL_LEFT, fx - W, 0);
	layer(UI_PANEL_RIGHT, fx + W, 0);
	layer(UI_PANEL_BOTTOM, 0, fy + H);
	layer(UI_PANEL_FACE, fx, fy);
	if (ui_modal())
		layer(UI_PANEL_OVERLAY, 0, 0);
}
