/* Screen stack geometry and gestures. The watchface's origin (fx, fy) is
 * the single state the layout follows: at rest it is (0, 0) on the
 * watchface, (W, 0) with the notifications on screen, (-W, 0) for
 * Today, (0, -H) for the launcher and (0, H) for the quick settings; the
 * side screens sit one screen away from the watchface. An open app lies
 * over all of them, sliding in from the right and fading in over
 * APP_FADE_MS when opened; dragged from the top edge, or from the left
 * edge on its first page, it follows the finger, fading, and let go
 * past 15 % of the way from where the drag began to the far edge it
 * closes onto the watchface. From the left edge on a later page only
 * that page follows the finger, over the page below (ui_app_peek), and
 * let go past 15 % it goes a page back. Put to sleep, the stack goes
 * back to the watchface but keeps the app for APP_KEEP_MS, so a quick
 * wake returns to it.
 *
 * Swipes between screens follow AsteroidOS's gesture filter: the
 * average move per touch event decides, taking the drag once it passes
 * 1 % of the screen along one axis (and the finger is LOCK_PX away).
 * The origin then moves with the finger, clamped to the screens that
 * exist in that direction, and a release past half the screen
 * (vertically only while still moving that way) or at more than
 * FLICK_PX per event switches screens, sliding linearly over SLIDE_MS. */
#include "asteroid.h"
#include "display/display.h"
#include "gpu/draw.h"
#include "ui/ui.h"
#include "ui/stack.h"

#define SWIPE_PX	40
#define LOCK_PX		8
#define FLICK_PX	10.0f
#define SLIDE_MS	100
#define DIM		0.4f
#define FADE_MS		300
#define APP_FADE_MS	100
#define APP_SNAP_MS	200
#define APP_KEEP_MS	5000
#define IND_NEAR	4.8f
#define EDGE_PX		40
#define BAND		16
#define BANDS		32

enum { IDLE, PENDING, SCROLL, SWIPE };
enum { LEFT, RIGHT, UP, DOWN };

static const bool scrolls[UI_PANELS] = { [UI_PANEL_LEFT] = true, [UI_PANEL_APP] = true };
static const bool pages[UI_PANELS] = { [UI_PANEL_BOTTOM] = true, [UI_PANEL_TOP] = true };

static unsigned launcher;
static int W, H, fx, fy, from_x, from_y, off, from_off, peek;
static unsigned screen, gesture;
static bool animating, dragging, horizontal, app_open, app_closing, app_top, peeking, peek_back, opening;
static uint32_t app_t0, slept_t;
static int down_x, down_y, prev_x, prev_y, moves, depth;
static uint32_t anim_t0, fade_t;
static float fade, vx, vy;
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

static void close_app(void)
{
	app_open = false;
	ui_set_app(-1);
}

void stack_home(void)
{
	screen = UI_PANEL_FACE;
	fx = fy = 0;
	if (peeking)
		ui_app_peek_end(false);
	if (app_open)
		close_app();
	animating = dragging = app_closing = peeking = opening = false;
	gesture = IDLE;
	fade = 0.0f;
}

void stack_sleep(uint32_t now_ms)
{
	bool open = app_open;
	app_open = false;
	stack_home();
	app_open = open;
	slept_t = now_ms;
	ui_set_screen(screen);
}

void stack_wake(uint32_t now_ms)
{
	if (app_open && now_ms - slept_t >= APP_KEEP_MS)
		close_app();
}

/* The app moves back to rest, or (close) away and onto the watchface;
 * while peeking its top page moves back, or (close) away and a page
 * back. */
static void slide_app(bool close)
{
	from_off = peeking ? peek : off;
	peek_back = close;
	anim_t0 = time_ms();
	animating = true;
	dragging = false;
	if (peeking)
		return;
	app_closing = close;
	if (close) {
		screen = UI_PANEL_FACE;
		fx = fy = 0;
	}
}

void stack_open_app(void)
{
	app_open = opening = true;
	app_closing = app_top = false;
	off = W;
	depth = 0;
	app_t0 = time_ms();
	stack_indicators();
}

static float app_alpha(void)
{
	uint32_t t = time_ms() - app_t0;
	float a = t >= APP_FADE_MS ? 1.0f : (float)t / APP_FADE_MS, size = app_top ? H : W;
	float b = (size - 2.0f * off) / size;
	if (opening)
		return a;
	return b < a ? (b > 0.0f ? b : 0.0f) : a;
}

unsigned stack_screen(void) { return screen; }
bool stack_moving(void) { return animating || dragging || opening; }
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

void stack_set_launcher(unsigned layout)
{
	launcher = layout;
}

/* Whether a drag on the launcher is its own. The horizontal layouts
 * take sideways drags. The vertical ones take vertical drags, except a
 * drag down from their start, or for the vertical pages from the top
 * edge, which goes back to the watchface. The speed dial takes drags
 * that start on its wheel. */
static bool launcher_scroll(int dx, int dy)
{
	if (launcher == 0 || launcher == 2)
		return absi(dx) > absi(dy);
	if (launcher == 6) {
		int x = down_x - W / 2, y = down_y - H / 2, r2 = x * x + y * y, lo = W * 24 / 100, hi = W * 47 / 100;
		return r2 > lo * lo && r2 < hi * hi;
	}
	if (absi(dx) > absi(dy))
		return false;
	return !(dy > 0 && ((launcher == 1 && down_y < EDGE_PX) || ui_launcher_edges() & 16));
}

static void release(void)
{
	int x = fx - rest_x(screen), y = fy - rest_y(screen), d = horizontal ? x : y;
	float v = horizontal ? vx : vy;
	bool half = absi(d) * 2 > (horizontal ? W : H) && (horizontal || (d > 0) == (v > 0));
	bool flick = (v > FLICK_PX && d > 0) || (v < -FLICK_PX && d < 0);
	if (half || flick)
		swipe(horizontal ? (x > 0 ? RIGHT : LEFT) : (y < 0 ? UP : DOWN));
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
				opening = false;
				cancel_press();
				if (peeking)
					ui_app_peek_end(false);
				peeking = !down && ui_app_depth();
				if (!peeking) {
					screen = UI_PANEL_FACE;
					fx = fy = 0;
				}
			}
		}
		if (gesture != SWIPE) {
			ui_touch(UI_PANEL_APP, kind, x, y);
			break;
		}
		int d = app_top ? clampi(dy, 0, H) : clampi(dx, 0, W);
		if (peeking)
			ui_app_peek(peek = d);
		else
			off = d;
		animating = false;
		dragging = kind == UI_TOUCH_MOVE;
		if (kind == UI_TOUCH_UP)
			slide_app(d * 100 > (app_top ? H - down_y : W - down_x) * 15);
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
	if (screen == UI_PANEL_BOTTOM)
		scroll = launcher_scroll(dx, dy);
	else if (screen == UI_PANEL_LEFT && absi(dx) > absi(dy))
		scroll = !(dx < 0 && ui_notes_edges() & 16);
	if (kind == UI_TOUCH_DOWN) {
		moves = 0;
		vx = vy = 0.0f;
	} else {
		moves++;
		vx += ((float)(x - prev_x) - vx) / moves;
		vy += ((float)(y - prev_y) - vy) / moves;
	}
	prev_x = x;
	prev_y = y;
	float lock = W / 100.0f;
	bool locked = (vx > lock || vx < -lock || vy > lock || vy < -lock) && dx * dx + dy * dy > LOCK_PX * LOCK_PX;
	if (app_open) {
		if (kind == UI_TOUCH_DOWN) {
			gesture = PENDING;
			down_x = x;
			down_y = y;
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
		ui_touch(screen, UI_TOUCH_DOWN, x, y);
		break;
	case UI_TOUCH_MOVE:
		if (gesture == PENDING && locked && !scroll) {
			gesture = SWIPE;
			horizontal = (vx < 0 ? -vx : vx) >= (vy < 0 ? -vy : vy);
			cancel_press();
			drag(dx, dy);
		} else if (gesture == SWIPE) {
			drag(dx, dy);
		} else if (gesture == PENDING || gesture == SCROLL) {
			if (far && scroll)
				gesture = SCROLL;
			ui_touch(screen, UI_TOUCH_MOVE, x, y);
		}
		break;
	case UI_TOUCH_UP:
		stack_indicators();
		if (gesture == SWIPE) {
			drag(dx, dy);
			release();
		} else if (gesture == PENDING && locked && !scroll) {
			cancel_press();
			horizontal = (vx < 0 ? -vx : vx) >= (vy < 0 ? -vy : vy);
			drag(dx, dy);
			release();
		} else if (gesture == PENDING && far && !scroll) {
			cancel_press();
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
	static unsigned shown = UI_PANELS;
	if (screen != shown)
		ui_set_screen(shown = screen);
	/* The app's indicators pop out again as it goes a page in or out. */
	if (app_open && ui_app_depth() != depth) {
		depth = ui_app_depth();
		stack_indicators();
	}
	if (opening) {
		uint32_t t = now_ms - app_t0;
		off = t >= APP_FADE_MS ? 0 : W - (int)(W * t / APP_FADE_MS);
		opening = t < APP_FADE_MS;
	}
	if (!animating)
		return;
	uint32_t t = now_ms - anim_t0;
	if (peeking) {
		float u = t >= APP_SNAP_MS ? 0.0f : 1.0f - (float)t / APP_SNAP_MS, p = 1.0f - u * u * u * u * u;
		ui_app_peek(peek = from_off + (int)(((peek_back ? W : 0) - from_off) * p));
		if (t >= APP_SNAP_MS) {
			animating = peeking = false;
			ui_app_peek_end(peek_back);
		}
		return;
	}
	if (app_open) {
		float u = t >= APP_SNAP_MS ? 0.0f : 1.0f - (float)t / APP_SNAP_MS, p = 1.0f - u * u * u * u * u;
		int to = app_closing ? (app_top ? H : W) / 2 : 0;
		off = from_off + (int)((to - from_off) * p);
		if (t >= APP_SNAP_MS) {
			animating = false;
			if (app_closing)
				close_app();
		}
		return;
	}
	if (t >= SLIDE_MS) {
		fx = rest_x(screen);
		fy = rest_y(screen);
		animating = false;
		return;
	}
	float p = (float)t / SLIDE_MS;
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
 * darkening (stack_launcher), except for the turbo list and the three by
 * three grid, which keep the watchface's. */
float stack_dim(void)
{
	bool dark = launcher == 4 || launcher == 5;
	float dx = (float)absi(fx) / W, dy = fy > 0 || dark ? (float)absi(fy) / H : 0.0f, d = dx > dy ? dx : dy;
	if (app_open && app_alpha() > d)
		d = app_alpha();
	return d * DIM;
}

float stack_launcher(void)
{
	return fy < 0 ? (float)-fy / H : 0.0f;
}

/* Edge indicators, AsteroidOS's: a light square turned 45 degrees,
 * centred on the middle of each edge with a screen beyond it, 7.2 px wide
 * at 60 % opacity. Each swipe and each wake they grow to 12 px and 80 %
 * and move 12 px in over 300 ms, then settle back over 400 ms. An open
 * app has them on its left (back) and top (close) edges, moving only
 * 4.8 px in, as it opens and goes a page in or out. While notifications
 * are waiting the left one stays out, on the watchface and on the
 * always-on face. Drawn from a 32 px texture made as they change. */
#define IND_TEX		32
#define IND_LOW_W	7.2f
#define IND_HIGH_W	12.0f
#define IND_LOW_A	0.6f
#define IND_HIGH_A	0.8f
#define IND_FAR		12.0f
#define IND_OUT_MS	300
#define IND_BACK_MS	400

static uint32_t ind_t0 = 0x80000000u;
static bool pending;

void stack_indicators(void)
{
	ind_t0 = time_ms();
}

/* Whether anything the stack draws still changes by itself: a slide, a
 * drag, an app opening, the dimming behind a pop-up or the indicators. */
bool stack_animating(uint32_t now_ms)
{
	return stack_moving() || (fade > 0.0f && fade < 1.0f) || now_ms - ind_t0 < IND_OUT_MS + IND_BACK_MS;
}

void stack_pending(bool on)
{
	pending = on;
}

/* How far they move in at the peak: less on an app. */
static float ind_far(void)
{
	return app_open ? IND_NEAR : IND_FAR;
}

/* How far into the pop-out animation, 0 at rest and 1 at its peak. */
static float ind_phase(void)
{
	uint32_t t = time_ms() - ind_t0;
	if (t < IND_OUT_MS)
		return (float)t / IND_OUT_MS;
	if (t < IND_OUT_MS + IND_BACK_MS)
		return 1.0f - (float)(t - IND_OUT_MS) / IND_BACK_MS;
	return 0.0f;
}

static void diamond(uint32_t *tex, unsigned stride, float w, float alpha)
{
	float h = w * 0.70710678f;
	for (int y = 0; y < IND_TEX; y++)
		for (int x = 0; x < IND_TEX; x++) {
			float dx = x + 0.5f - IND_TEX / 2, dy = y + 0.5f - IND_TEX / 2;
			float c = h - ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy)) + 0.5f;
			uint32_t v = (uint32_t)((c < 0 ? 0 : c > 1 ? 1 : c) * alpha * 255.0f + 0.5f);
			tex[y * stride + x] = v * 0x01010101u;
		}
}

/* Edges to show: left, right, top, bottom as bits 0-3. */
static unsigned ind_edges(void)
{
	if (app_open)
		return 1 | 4;
	switch (screen) {
	case UI_PANEL_FACE:
		return 0xf;
	case UI_PANEL_LEFT:
		return ui_notes_edges() & 0xf;
	case UI_PANEL_RIGHT:
		return 1;
	case UI_PANEL_BOTTOM:
		return ui_launcher_edges() & 0xf;
	case UI_PANEL_TOP:
		return 8;
	}
	return 0;
}

static void indicators(void)
{
	static uint32_t *tex[2];
	static float shown[2][2];
	if (!tex[0])
		for (int i = 0; i < 2; i++)
			tex[i] = gpu_alloc(IND_TEX * IND_TEX * 4, 64);
	if (!tex[1])
		return;
	unsigned edges = ind_edges();
	if (!edges || fade > 0.0f)
		return;
	float p = ind_phase();
	for (int e = 0; e < 4; e++) {
		if (!(edges & 1u << e))
			continue;
		bool kept = e == 0 && pending && screen == UI_PANEL_FACE;
		float q = kept ? 1.0f : p;
		float w = IND_LOW_W + (IND_HIGH_W - IND_LOW_W) * q, a = IND_LOW_A + (IND_HIGH_A - IND_LOW_A) * q;
		float in = kept ? IND_FAR : ind_far() * p;
		int k = kept ? 1 : 0;
		if (shown[k][0] != w || shown[k][1] != a) {
			diamond(tex[k], IND_TEX, w, a);
			shown[k][0] = w;
			shown[k][1] = a;
		}
		int cx = e == 0 ? (int)in : e == 1 ? W - (int)in : W / 2;
		int cy = e == 2 ? (int)in : e == 3 ? H - (int)in : H / 2;
		gpu_draw_layer_at((uint32_t)(uintptr_t)tex[k], IND_TEX, IND_TEX, IND_TEX * 4, cx - IND_TEX / 2, cy - IND_TEX / 2,
				  1.0f);
	}
}

/* The left indicator on the always-on face, drawn on the CPU into an
 * XRGB8888 buffer, while notifications are waiting, moved by (dx, dy). */
void stack_indicators_cpu(uint32_t *buf, uint32_t stride, int dx, int dy)
{
	static uint32_t tex[IND_TEX * IND_TEX];
	if (!pending)
		return;
	diamond(tex, IND_TEX, IND_HIGH_W, IND_HIGH_A);
	int x0 = (int)IND_FAR - IND_TEX / 2 + dx, y0 = H / 2 - IND_TEX / 2 + dy;
	for (int y = 0; y < IND_TEX; y++)
		for (int x = 0; x < IND_TEX; x++) {
			int px = x0 + x;
			if (px < 0)
				continue;
			uint32_t a = tex[y * IND_TEX + x] & 0xff, d = buf[(y0 + y) * stride + px], o = 0;
			for (int s = 0; s < 24; s += 8)
				o |= ((d >> s & 0xff) * (255 - a) / 255 + a) << s;
			buf[(y0 + y) * stride + px] = o;
		}
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
	indicators();
	if (fade > 0.0f)
		layer(UI_PANEL_OVERLAY, 0, 0, fade);
}
