/* Screen stack geometry and gestures. The watchface's origin (fx, fy) is
 * the single state the layout follows: at rest it is (0, 0) on the
 * watchface, (W, 0) with the notifications on screen, (-W, 0) for
 * Today, (0, -H) for the launcher and (0, H) for the quick settings; the
 * side screens sit one screen away from the watchface. An open app lies
 * over all of them, fading in when opened; dragged from the left or top
 * edge it follows the finger, fading, and let go past 15 % it goes a
 * page back, or closes onto the watchface from its first page or from
 * the top edge. A drag moves the origin with the finger,
 * clamped to the screens that exist in that direction, and a release
 * past half the screen or a quick flick switches screens; the slide back
 * to rest is an ease-out over SLIDE_MS. */
#include "asteroid.h"
#include "display/display.h"
#include "gpu/draw.h"
#include "ui/ui.h"
#include "ui/stack.h"

#define SWIPE_PX	40
#define FLICK_MS	300
#define SLIDE_MS	100
#define DIM		0.4f
#define FADE_MS		150
#define APP_FADE_MS	100
#define APP_SNAP_MS	200
#define EDGE_PX		40
#define BAND		16
#define BANDS		32

enum { IDLE, PENDING, SCROLL, SWIPE };
enum { LEFT, RIGHT, UP, DOWN };

static const bool scrolls[UI_PANELS] = { [UI_PANEL_LEFT] = true, [UI_PANEL_APP] = true };
static const bool pages[UI_PANELS] = { [UI_PANEL_BOTTOM] = true, [UI_PANEL_TOP] = true };

static int W, H, fx, fy, from_x, from_y, off, from_off;
static unsigned screen, gesture;
static bool animating, dragging, horizontal, app_open, app_closing, app_top;
static uint32_t app_t0;
static int down_x, down_y;
static uint32_t down_ms, anim_t0, fade_t;
static float fade;
static struct { int x0, x1; } bands[UI_PANELS][BANDS];

static int rest_x(unsigned s) { return s == UI_PANEL_LEFT ? W : s == UI_PANEL_RIGHT ? -W : 0; }
static int rest_y(unsigned s) { return s == UI_PANEL_BOTTOM ? -H : s == UI_PANEL_TOP ? H : 0; }
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
	animating = dragging = app_open = app_closing = false;
	gesture = IDLE;
	fade = 0.0f;
}

/* The app moves back to rest, or (close) away and onto the watchface. */
static void slide_app(bool close)
{
	from_off = off;
	app_closing = close;
	anim_t0 = time_ms();
	animating = true;
	dragging = false;
	if (close) {
		screen = UI_PANEL_FACE;
		fx = fy = 0;
	}
}

void stack_open_app(void)
{
	app_open = true;
	app_closing = false;
	off = 0;
	app_t0 = time_ms();
}

static float app_alpha(void)
{
	uint32_t t = time_ms() - app_t0;
	float a = t >= APP_FADE_MS ? 1.0f : (float)t / APP_FADE_MS, size = app_top ? H : W;
	float b = (size - 2.0f * off) / size;
	return b < a ? (b > 0.0f ? b : 0.0f) : a;
}

unsigned stack_screen(void) { return screen; }
bool stack_moving(void) { return animating || dragging || (app_open && time_ms() - app_t0 < APP_FADE_MS); }
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
		s = dir == RIGHT ? UI_PANEL_LEFT : dir == LEFT ? UI_PANEL_RIGHT : dir == UP ? UI_PANEL_BOTTOM : UI_PANEL_TOP;
	else if ((screen == UI_PANEL_LEFT && dir == LEFT) || (screen == UI_PANEL_RIGHT && dir == RIGHT) ||
		 (screen == UI_PANEL_BOTTOM && dir == DOWN) || (screen == UI_PANEL_TOP && dir == UP))
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
		dy = clampi(dy, -H, H);
		break;
	case UI_PANEL_TOP:
		dx = 0;
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
	if (absi(x) * 2 > W || absi(y) * 2 > H || (flick && (x || y)))
		swipe(absi(x) >= absi(y) ? (x > 0 ? RIGHT : LEFT) : (y < 0 ? UP : DOWN));
	else
		slide_to(screen);
}

static void cancel_press(void)
{
	ui_touch(app_open ? UI_PANEL_APP : screen, UI_TOUCH_UP, -10000, -10000);
}

/* On an open app only a drag that starts at the left edge going right
 * (back) or at the top edge going down (close) moves the app; anything
 * else is the app's. */
static void touch_app(int kind, int x, int y, int dx, int dy, bool far)
{
	static bool from_left, from_top;
	switch (kind) {
	case UI_TOUCH_DOWN:
		from_left = x < EDGE_PX;
		from_top = y < EDGE_PX;
		ui_touch(UI_PANEL_APP, kind, x, y);
		break;
	case UI_TOUCH_MOVE:
	case UI_TOUCH_UP:
		if (gesture == PENDING && far) {
			bool back = from_left && dx > 0 && absi(dx) > absi(dy), down = from_top && dy > 0 && absi(dy) > absi(dx);
			gesture = back || down ? SWIPE : SCROLL;
			if (gesture == SWIPE) {
				app_top = down;
				cancel_press();
				if (down || !ui_app_depth()) {
					screen = UI_PANEL_FACE;
					fx = fy = 0;
				}
			}
		}
		if (gesture != SWIPE) {
			ui_touch(UI_PANEL_APP, kind, x, y);
			break;
		}
		off = app_top ? clampi(dy, 0, H) : clampi(dx, 0, W);
		animating = false;
		dragging = kind == UI_TOUCH_MOVE;
		if (kind == UI_TOUCH_UP) {
			if (off * 100 <= (app_top ? H : W) * 15)
				slide_app(false);
			else if (!app_top && ui_app_back())
				slide_app(false);
			else
				slide_app(true);
		}
		break;
	}
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
	bool scroll = (absi(dy) >= absi(dx) && scrolls[screen]) || (absi(dx) > absi(dy) && pages[screen]);
	bool flick = now_ms - down_ms < FLICK_MS;
	if (app_open) {
		if (kind == UI_TOUCH_DOWN) {
			gesture = PENDING;
			down_x = x;
			down_y = y;
			down_ms = now_ms;
		}
		touch_app(kind, x, y, dx, dy, far);
		if (kind == UI_TOUCH_UP)
			gesture = IDLE;
		return;
	}
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
	float step = (float)(now_ms - fade_t) / FADE_MS;
	fade_t = now_ms;
	if (ui_modal())
		fade = fade + step < 1.0f ? fade + step : 1.0f;
	else
		fade = fade - step > 0.0f ? fade - step : 0.0f;
	if (!animating)
		return;
	uint32_t t = now_ms - anim_t0;
	if (app_open) {
		float u = t >= APP_SNAP_MS ? 0.0f : 1.0f - (float)t / APP_SNAP_MS, p = 1.0f - u * u * u * u * u;
		int to = app_closing ? (app_top ? H : W) / 2 : 0;
		off = from_off + (int)((to - from_off) * p);
		if (t >= APP_SNAP_MS) {
			animating = false;
			app_open = !app_closing;
		}
		return;
	}
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

/* Each band of BAND rows keeps the columns that may hold content: what
 * a repaint covering the whole band found, widened by later repaints of
 * part of it. */
void stack_damage(unsigned panel, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	const uint32_t *px = (const uint32_t *)(uintptr_t)(UI_LAYER_BASE + panel * UI_LAYER_STRIDE);
	for (uint32_t b = y / BAND; b < BANDS && b * BAND < y + h && b * BAND < (uint32_t)H; b++) {
		uint32_t r0 = b * BAND, r1 = r0 + BAND < (uint32_t)H ? r0 + BAND : (uint32_t)H;
		bool whole = x == 0 && w >= (uint32_t)W && y <= r0 && y + h >= r1;
		int x0 = W, x1 = 0;
		for (uint32_t r = r0 > y ? r0 : y; r < r1 && r < y + h; r++) {
			const uint32_t *row = px + r * fb.stride;
			for (uint32_t c = x; c < x + w && (int)c < x0; c++)
				if (row[c]) {
					x0 = c;
					break;
				}
			for (uint32_t c = x + w; c > x && (int)c > x1; c--)
				if (row[c - 1]) {
					x1 = c;
					break;
				}
		}
		if (!whole && bands[panel][b].x0 < bands[panel][b].x1) {
			if (bands[panel][b].x0 < x0)
				x0 = bands[panel][b].x0;
			if (bands[panel][b].x1 > x1)
				x1 = bands[panel][b].x1;
		}
		bands[panel][b].x0 = x0;
		bands[panel][b].x1 = x1;
	}
}

bool stack_overlay_frozen(void)
{
	return !ui_modal() && fade > 0.0f;
}

static void layer(unsigned panel, int x, int y, float alpha)
{
	if (x <= -W || x >= W || y <= -H || y >= H)
		return;
	struct gpu_rect parts[BANDS];
	unsigned n = 0;
	for (int b = 0; b < BANDS && b * BAND < H; b++) {
		int x0 = bands[panel][b].x0, x1 = bands[panel][b].x1;
		if (x0 >= x1)
			continue;
		int r1 = (b + 1) * BAND < H ? (b + 1) * BAND : H;
		if (n && parts[n - 1].x == x0 && parts[n - 1].x + (int)parts[n - 1].width == x1 &&
		    parts[n - 1].y + (int)parts[n - 1].height == b * BAND)
			parts[n - 1].height = r1 - parts[n - 1].y;
		else
			parts[n++] = (struct gpu_rect){ x0, b * BAND, x1 - x0, r1 - b * BAND };
	}
	gpu_draw_layer_parts(UI_LAYER_BASE + panel * UI_LAYER_STRIDE, W, H, fb.stride * 4, x, y, alpha, parts, n);
}

/* Towards the launcher the mesh takes the launcher's colours instead of
 * darkening (stack_launcher). */
float stack_dim(void)
{
	float dx = (float)absi(fx) / W, dy = fy > 0 ? (float)fy / H : 0.0f, d = dx > dy ? dx : dy;
	if (app_open && app_alpha() > d)
		d = app_alpha();
	return d * DIM;
}

float stack_launcher(void)
{
	return fy < 0 ? (float)-fy / H : 0.0f;
}

void stack_compose(void (*background)(int x, int y, float alpha))
{
	/* An app is drawn over its own mesh (background), both moved and
	 * faded together over the screens behind. */
	float a = app_open ? app_alpha() : 0.0f;
	if (a < 1.0f) {
		layer(UI_PANEL_LEFT, fx - W, 0, 1.0f);
		layer(UI_PANEL_RIGHT, fx + W, 0, 1.0f);
		layer(UI_PANEL_BOTTOM, 0, fy + H, 1.0f);
		layer(UI_PANEL_TOP, 0, fy - H, 1.0f);
		layer(UI_PANEL_FACE, fx, fy, 1.0f);
	}
	if (app_open) {
		int x = app_top ? 0 : off, y = app_top ? off : 0;
		background(x, y, a);
		layer(UI_PANEL_APP, x, y, a);
	}
	if (fade > 0.0f)
		layer(UI_PANEL_OVERLAY, 0, 0, fade);
}
