/* The Cortex-M build's backdrop (flatmesh_mcu.h). The mesh is the one
 * flatmesh.c draws at time 0, baked by tools/ui-mcu/flatmesh.py. Each
 * line fills with the outer colour, then each triangle crossing the
 * line's pixel centres fills its run; shared edges are worked out from
 * the same two corners either side, so neighbours meet without gaps.
 * Triangles come top first: those a line crosses are kept in a list as
 * the lines go down, and it starts over when a line is above the last. */
/* The mesh is a third of a whole draw: -O2 takes 5 % more off it. */
#pragma GCC optimize("O2")
#include "flatmesh_mcu.h"
#include "ui/ui.h"
#include "flatmesh_mcu_data.h"

#define CENTER		0x888888u
#define OUTER		0x000000u
/* The view as the stack has it at rest beside the watchface: 5 % of the
 * screen over and 40 % darker, in 1/256. */
#define MOVE_PCT	5
#define DIMMED		154
/* Under the notification pop-up, as its #000000cc leaves it. */
#define POPUP_SHADE	51
#define IND_LOW_W	7.2f
#define IND_HIGH_W	12.0f
#define IND_LOW_A	0.6f
#define IND_HIGH_A	0.8f
#define IND_FAR		12
/* The strip's dots: diamonds along the bottom, the page on show large
 * and bright, the one a drag would turn to large. */
#define DOTS		12
#define DOT_GAP		16.0f
#define DOT_UP		14
#define DOT_W		5.0f
#define DOT_A		0.4f
#define DOT_ON_W	7.0f
#define DOT_ON_A	0.9f
#define DOT_NEXT_A	0.6f

typedef uint32_t __attribute__((may_alias)) word;

static int screen_w, screen_h;
/* Corners in 1/16 px. */
static int16_t at[FLATMESH_MCU_POINTS][2];
/* Each triangle's corners and the slopes of its edges a-c, a-b and b-c in
 * 1/65536, so a line takes a multiply per edge and no divide. */
static struct tri {
	int16_t ax, ay, bx, by, cy;
	int32_t ac, ab, bc;
} tris[FLATMESH_MCU_TRIANGLES];
static uint16_t colors[FLATMESH_MCU_TRIANGLES], outer;
static unsigned mixed;
static uint8_t crossing[FLATMESH_MCU_CROSSING + 4];
static unsigned n_crossing, next;
static int last_y;

static struct view {
	bool mesh;
	int dx, dy;
	unsigned shade;
} view;

struct ind {
	float x, y, w, a;
};
static struct ind inds[4 + DOTS];
static unsigned n_inds;

/* x in 1/65536 of 1/16 px per 1/16 px down the edge from a to b. */
static int32_t slope(const int16_t *a, const int16_t *b)
{
	return b[1] > a[1] ? (b[0] - a[0]) * 65536 / (b[1] - a[1]) : 0;
}

void flatmesh_mcu_init(uint32_t width, uint32_t height)
{
	screen_w = (int)width;
	screen_h = (int)height;
	for (int i = 0; i < FLATMESH_MCU_POINTS; i++) {
		at[i][0] = (int16_t)(flatmesh_mcu_points[i][0] * screen_w * 16 / 8192);
		at[i][1] = (int16_t)(flatmesh_mcu_points[i][1] * screen_h * 16 / 8192);
	}
	for (int t = 0; t < FLATMESH_MCU_TRIANGLES; t++) {
		const int16_t *a = at[flatmesh_mcu_triangles[t][0]], *b = at[flatmesh_mcu_triangles[t][1]],
			      *c = at[flatmesh_mcu_triangles[t][2]];
		tris[t] = (struct tri){ a[0], a[1], b[0], b[1], c[1], slope(a, c), slope(a, b), slope(b, c) };
	}
}

/* RGB565 of 0xRRGGBB times shade/256; a grey keeps green at red's and
 * blue's depth, or neighbouring shades of it would differ in hue. */
static uint16_t rgb565(uint32_t c, unsigned shade)
{
	unsigned r = (c >> 16 & 0xff) * shade >> 8, g = (c >> 8 & 0xff) * shade >> 8, b = (c & 0xff) * shade >> 8;
	unsigned g6 = r == g && g == b ? (r >> 3) << 1 | r >> 7 : g >> 2;
	return (uint16_t)((r >> 3) << 11 | g6 << 5 | b >> 3);
}

static void mix(unsigned shade)
{
	for (int t = 0; t < FLATMESH_MCU_TRIANGLES; t++) {
		unsigned s = flatmesh_mcu_triangles[t][3], c = 0;
		for (int k = 0; k < 24; k += 8)
			c |= ((CENTER >> k & 0xff) * s + (OUTER >> k & 0xff) * (255 - s) + 127) / 255 << k;
		colors[t] = rgb565(c, shade);
	}
	outer = rgb565(OUTER, shade);
}

static void fill(uint16_t *p, int n, uint16_t c)
{
	if (n > 0 && ((uintptr_t)p & 2)) {
		*p++ = c;
		n--;
	}
	word *q = (word *)p, cc = c * 0x10001u;
	for (; n >= 8; n -= 8, q += 4)
		q[0] = q[1] = q[2] = q[3] = cc;
	for (; n >= 2; n -= 2)
		*q++ = cc;
	if (n)
		*(uint16_t *)q = c;
}

/* x in 1/16 px dy down from x along an edge of slope s. */
static inline int along(int x, int32_t s, int dy)
{
	return x + (int)(((int64_t)s * dy) >> 16);
}

/* The runs are put in order of x, and the list in that order for the
 * next line, where it mostly still holds. */
static void mesh_line(uint16_t *px, int y, int x0, int w)
{
	/* from in the high half, so runs compare as one number. */
	uint32_t runs[sizeof(crossing)];
	uint8_t ids[sizeof(crossing)], rest[sizeof(crossing)];
	unsigned n = 0, n_rest = 0, i = 0;
	int fy = (y - view.dy) * 16 + 8, ox = view.dx * 16 + 7 - x0 * 16;
	if (y < last_y)
		n_crossing = next = 0;
	last_y = y;
	for (;;) {
		unsigned k;
		if (i < n_crossing)
			k = crossing[i++];
		else if (next < FLATMESH_MCU_TRIANGLES && tris[next].ay <= fy)
			k = next++;
		else
			break;
		const struct tri *t = &tris[k];
		if (t->cy <= fy || n + n_rest == sizeof(crossing))
			continue;
		int l = along(t->ax, t->ac, fy - t->ay);
		int r = fy < t->by ? along(t->ax, t->ab, fy - t->ay) : along(t->bx, t->bc, fy - t->by);
		if (l > r) {
			int s = l;
			l = r;
			r = s;
		}
		int from = (l + ox) >> 4, to = (r + ox) >> 4;
		from = from < 0 ? 0 : from;
		to = to > w ? w : to;
		if (from >= to) {
			rest[n_rest++] = (uint8_t)k;
			continue;
		}
		uint32_t key = (uint32_t)from << 16 | (uint32_t)to;
		unsigned j = n++;
		for (; j && runs[j - 1] > key; j--) {
			runs[j] = runs[j - 1];
			ids[j] = ids[j - 1];
		}
		runs[j] = key;
		ids[j] = (uint8_t)k;
	}
	int x = 0;
	for (unsigned k = 0; k < n; k++) {
		int from = (int)(runs[k] >> 16), to = (int)(runs[k] & 0xffff);
		if (from < x)
			from = x;
		if (from > x)
			fill(px + x, from - x, outer);
		if (to > from) {
			fill(px + from, to - from, colors[ids[k]]);
			x = to;
		}
		crossing[k] = ids[k];
	}
	if (w > x)
		fill(px + x, w - x, outer);
	for (unsigned k = 0; k < n_rest; k++)
		crossing[n + k] = rest[k];
	n_crossing = n + n_rest;
}

void ui_line_under(uint32_t y, uint32_t x, uint32_t w, uint16_t *rgb565)
{
	if (view.mesh)
		mesh_line(rgb565, (int)y, (int)x, (int)w);
	else
		fill(rgb565, (int)w, 0);
}

/* AsteroidOS's indicator: a white square turned 45 degrees, w wide,
 * its edges smoothed over a pixel, at opacity a. */
static void ind_line(uint16_t *px, int y, int x0, int w, const struct ind *d)
{
	float h = d->w * 0.70710678f, dy = (float)y + 0.5f - d->y;
	if (dy < 0)
		dy = -dy;
	if (dy >= h + 0.5f)
		return;
	int from = (int)(d->x - h - 1.0f), to = (int)(d->x + h + 2.0f);
	if (from < x0)
		from = x0;
	if (to > x0 + w)
		to = x0 + w;
	for (int x = from; x < to; x++) {
		float dx = (float)x + 0.5f - d->x;
		float c = h - ((dx < 0 ? -dx : dx) + dy) + 0.5f;
		if (c <= 0)
			continue;
		unsigned a = (unsigned)((c > 1 ? 1 : c) * d->a * 255.0f + 0.5f), p = px[x - x0];
		unsigned r = p >> 11, g = p >> 5 & 63, b = p & 31;
		r += ((31 - r) * a + 127) / 255;
		g += ((63 - g) * a + 127) / 255;
		b += ((31 - b) * a + 127) / 255;
		px[x - x0] = (uint16_t)(r << 11 | g << 5 | b);
	}
}

void ui_line_over(uint32_t y, uint32_t x, uint32_t w, uint16_t *rgb565)
{
	for (unsigned i = 0; i < n_inds; i++)
		ind_line(rgb565, (int)y, (int)x, (int)w, &inds[i]);
}

static void ind_dirty(const struct ind *d)
{
	int r = (int)(d->w * 0.70710678f) + 2, x = (int)d->x - r, y = (int)d->y - r;
	int x1 = x + 2 * r, y1 = y + 2 * r;
	x = x < 0 ? 0 : x;
	y = y < 0 ? 0 : y;
	x1 = x1 > screen_w ? screen_w : x1;
	y1 = y1 > screen_h ? screen_h : y1;
	if (x < x1 && y < y1)
		ui_mark_dirty((uint32_t)x, (uint32_t)y, (uint32_t)(x1 - x), (uint32_t)(y1 - y));
}

static unsigned indicators(const struct backdrop *b, struct ind *out)
{
	unsigned n = 0;
	if (b->ambient) {
		if (b->pending)
			out[n++] = (struct ind){ (float)(IND_FAR + b->dx), (float)(screen_h / 2 + b->dy), IND_HIGH_W, IND_HIGH_A };
		return n;
	}
	if (b->modal)
		return 0;
	for (unsigned e = 0; e < 4; e++) {
		if (!(b->edges & 1u << e))
			continue;
		bool kept = e == 0 && b->pending && b->screen == UI_PANEL_FACE;
		float in = kept ? IND_FAR : 0.0f;
		out[n++] = (struct ind){ e == 0 ? in : e == 1 ? (float)screen_w - in : (float)(screen_w / 2),
					 e == 2 ? in : e == 3 ? (float)screen_h - in : (float)(screen_h / 2),
					 kept ? IND_HIGH_W : IND_LOW_W, kept ? IND_HIGH_A : IND_LOW_A };
	}
	for (unsigned i = 0; i < b->pages && i < DOTS; i++) {
		bool on = i == b->page, next = (int)i == b->next;
		out[n++] = (struct ind){ (float)screen_w / 2 + ((float)i - (float)(b->pages - 1) / 2) * DOT_GAP,
					 (float)(screen_h - DOT_UP), on || next ? DOT_ON_W : DOT_W,
					 on ? DOT_ON_A : next ? DOT_NEXT_A : DOT_A };
	}
	return n;
}

bool flatmesh_mcu_draw(unsigned panel, const struct backdrop *b, bool whole)
{
	int mx = screen_w * MOVE_PCT / 100, my = screen_h * MOVE_PCT / 100;
	unsigned s = b->screen;
	struct view v = { !b->ambient, s == UI_PANEL_LEFT ? mx : s == UI_PANEL_RIGHT ? -mx : 0,
			  s == UI_PANEL_TOP ? my : s == UI_PANEL_BOTTOM ? -my : 0,
			  s == UI_PANEL_LEFT || s == UI_PANEL_RIGHT || s == UI_PANEL_TOP ? DIMMED : 256 };
	if (b->popup)
		v.shade = v.shade * POPUP_SHADE / 255;
	if (v.mesh && v.shade != mixed) {
		mix(v.shade);
		mixed = v.shade;
	}
	if (v.mesh != view.mesh || v.dx != view.dx || v.dy != view.dy || v.shade != view.shade) {
		view = v;
		n_crossing = next = 0;
		whole = true;
	}
	struct ind now[4 + DOTS];
	unsigned n = indicators(b, now);
	bool same = n == n_inds;
	for (unsigned i = 0; i < n && same; i++)
		same = now[i].x == inds[i].x && now[i].y == inds[i].y && now[i].w == inds[i].w && now[i].a == inds[i].a;
	if (!same && !whole) {
		for (unsigned i = 0; i < n_inds; i++)
			ind_dirty(&inds[i]);
		for (unsigned i = 0; i < n; i++)
			ind_dirty(&now[i]);
	}
	for (unsigned i = 0; i < n; i++)
		inds[i] = now[i];
	n_inds = n;
	return ui_draw(panel, whole);
}
