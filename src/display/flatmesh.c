/* AsteroidOS's FlatMesh background drawn in software: a Delaunay mesh of
 * 100 points, each triangle a flat colour between the centre and outer
 * colours by its distance from the centre, every vertex drifting between
 * random shifts. The animation state advances 0.02 per 90 ms and the shift
 * pair rotates once per loop, as the QML element does. */
#include "asteroid.h"
#include "display.h"
#include "flatmesh_geometry.h"

#define SCALE 1.2f
#define DEFAULT_CENTER 0x888888u
#define DEFAULT_OUTER 0x000000u

static uint32_t colors[FLATMESH_INDICES / 3], center = DEFAULT_CENTER, outer = DEFAULT_OUTER;
static float view_scale = 1.0f, view_x, view_y, view_shade = 1.0f;

void flatmesh_view(float scale, float dx, float dy, float shade)
{
	view_scale = scale;
	view_x = dx;
	view_y = dy;
	view_shade = shade;
}
static int hashes[FLATMESH_INDICES];
static bool ready;

static void prepare(void)
{
	for (int i = 0; i < FLATMESH_INDICES; i++) {
		const float *v = flatmesh_vertices[flatmesh_indices[i]];
		hashes[i] = (int)(v[0] * 100.0f) + (int)(v[1] * 100.0f);
	}
	ready = true;
	flatmesh_colors(center, outer);
}

/* The centre and outer colours, 0xRRGGBB. */
void flatmesh_colors(uint32_t c, uint32_t o)
{
	center = c;
	outer = o;
	for (int t = 0; t < FLATMESH_INDICES / 3; t++) {
		float m = flatmesh_vertices[flatmesh_indices[3 * t]][2];
		uint32_t rgb = 0;
		for (int s = 0; s < 24; s += 8)
			rgb |= (uint32_t)((float)(c >> s & 0xff) * (1 - m) + (float)(o >> s & 0xff) * m) << s;
		colors[t] = rgb;
	}
}

static void span(uint32_t *row, int x0, int x1, uint32_t c)
{
	if (x0 < 0) x0 = 0;
	if (x1 > (int)fb.width) x1 = (int)fb.width;
	for (int x = x0; x < x1; x++)
		row[x] = c;
}

static void triangle(uint32_t *buf, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c)
{
	float t;
	if (y1 < y0) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
	if (y2 < y0) { t = x0; x0 = x2; x2 = t; t = y0; y0 = y2; y2 = t; }
	if (y2 < y1) { t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
	int ya = (int)y0, yb = (int)y1, yc = (int)y2;
	if (ya < 0) ya = 0;
	if (yc >= (int)fb.height) yc = (int)fb.height - 1;
	for (int y = ya; y <= yc; y++) {
		float fy = (float)y + 0.5f;
		if (fy < y0 || fy > y2 || y2 == y0)
			continue;
		float xl = x0 + (x2 - x0) * (fy - y0) / (y2 - y0);
		float xr;
		if (fy < y1 && y1 != y0)
			xr = x0 + (x1 - x0) * (fy - y0) / (y1 - y0);
		else if (y2 != y1)
			xr = x1 + (x2 - x1) * (fy - y1) / (y2 - y1);
		else
			xr = x1;
		if (xl > xr) { t = xl; xl = xr; xr = t; }
		span(buf + y * fb.stride, (int)(xl + 0.5f), (int)(xr + 0.5f), c);
	}
	(void)yb;
}

static void animate(uint32_t t_ms, void (*vertex)(int i, float x, float y, void *ctx), void *ctx)
{
	if (!ready)
		prepare();
	uint32_t step = t_ms / 90;
	int loop = (int)(step / 50);
	float state = (float)(step % 50) * 0.02f;
	for (int i = 0; i < FLATMESH_INDICES; i++) {
		const float *v = flatmesh_vertices[flatmesh_indices[i]];
		int idx = loop + hashes[i];
		int a = ((idx % FLATMESH_SHIFTS) + FLATMESH_SHIFTS) % FLATMESH_SHIFTS;
		int b = (a + 1) % FLATMESH_SHIFTS;
		float sx = flatmesh_shifts[a][0] + (flatmesh_shifts[b][0] - flatmesh_shifts[a][0]) * state;
		float sy = flatmesh_shifts[a][1] + (flatmesh_shifts[b][1] - flatmesh_shifts[a][1]) * state;
		vertex(i, (v[0] + sx) * SCALE + 0.5f, (v[1] + sy) * SCALE + 0.5f, ctx);
	}
}

static uint32_t shaded(uint32_t c)
{
	if (view_shade >= 1.0f)
		return c;
	return (uint32_t)(((c >> 16) & 0xff) * view_shade) << 16 | (uint32_t)(((c >> 8) & 0xff) * view_shade) << 8 |
	       (uint32_t)((c & 0xff) * view_shade);
}

struct cpu_ctx { uint32_t *buf; float px[3], py[3]; };

static void cpu_vertex(int i, float x, float y, void *ctx)
{
	struct cpu_ctx *c = ctx;
	x = 0.5f + (x - 0.5f) * view_scale + view_x;
	y = 0.5f + (y - 0.5f) * view_scale + view_y;
	c->px[i % 3] = x * (float)fb.width;
	c->py[i % 3] = y * (float)fb.height;
	if (i % 3 == 2)
		triangle(c->buf, c->px[0], c->py[0], c->px[1], c->py[1], c->px[2], c->py[2], shaded(colors[i / 3]));
}

void flatmesh_render(uint32_t *buf, uint32_t t_ms)
{
	struct cpu_ctx c = { .buf = buf };
	animate(t_ms, cpu_vertex, &c);
}

/* For the GPU: interleaved {x, y, 0, 1, r, g, b, 1} per vertex in clip
 * space, row 0 of the target at the top. */
static void gpu_vertex(int i, float x, float y, void *ctx)
{
	float *o = (float *)ctx + i * 8;
	uint32_t c = colors[i / 3];
	x = 0.5f + (x - 0.5f) * view_scale + view_x;
	y = 0.5f + (y - 0.5f) * view_scale + view_y;
	o[0] = x * 2.0f - 1.0f;
	o[1] = 1.0f - y * 2.0f;
	o[2] = 0.0f;
	o[3] = 1.0f;
	o[4] = (float)((c >> 16) & 0xff) / 255.0f * view_shade;
	o[5] = (float)((c >> 8) & 0xff) / 255.0f * view_shade;
	o[6] = (float)(c & 0xff) / 255.0f * view_shade;
	o[7] = 1.0f;
}

unsigned flatmesh_vertices_gpu(float *out, uint32_t t_ms)
{
	animate(t_ms, gpu_vertex, out);
	return FLATMESH_INDICES;
}

/* A second mesh, an app's own background: its colours, moved by (dx, dy)
 * of the screen, premultiplied by alpha for blending over the first, and
 * cut to the round window (a 64-gon) around its moved centre. Triangles
 * inside it pass whole, those across its edge are clipped polygon by
 * polygon and fanned out again. */
#define ROUND_SIDES 64

struct look { float pos[FLATMESH_INDICES][2]; float dx, dy; };

static void look_vertex(int i, float x, float y, void *ctx)
{
	struct look *l = ctx;
	l->pos[i][0] = 0.5f + (x - 0.5f) * view_scale + l->dx;
	l->pos[i][1] = 0.5f + (y - 0.5f) * view_scale + l->dy;
}

static unsigned clip_round(float (*p)[2], unsigned n, float cx, float cy)
{
	/* cos and sin of 2 pi / 64, stepped around from (0.5, 0). */
	static float edge[ROUND_SIDES][2];
	if (!edge[0][0]) {
		const float c = 0.99518473f, s = 0.09801714f;
		edge[0][0] = 0.5f;
		for (int k = 1; k < ROUND_SIDES; k++) {
			edge[k][0] = edge[k - 1][0] * c - edge[k - 1][1] * s;
			edge[k][1] = edge[k - 1][0] * s + edge[k - 1][1] * c;
		}
	}
	float tmp[ROUND_SIDES + 3][2];
	for (int k = 0; k < ROUND_SIDES && n; k++) {
		float ax = cx + edge[k][0], ay = cy + edge[k][1];
		float bx = cx + edge[(k + 1) % ROUND_SIDES][0], by = cy + edge[(k + 1) % ROUND_SIDES][1];
		unsigned m = 0;
		for (unsigned j = 0; j < n; j++) {
			float *u = p[j], *v = p[(j + 1) % n];
			float su = (bx - ax) * (u[1] - ay) - (by - ay) * (u[0] - ax);
			float sv = (bx - ax) * (v[1] - ay) - (by - ay) * (v[0] - ax);
			if (su >= 0) {
				tmp[m][0] = u[0];
				tmp[m++][1] = u[1];
			}
			if ((su >= 0) != (sv >= 0)) {
				float t = su / (su - sv);
				tmp[m][0] = u[0] + (v[0] - u[0]) * t;
				tmp[m++][1] = u[1] + (v[1] - u[1]) * t;
			}
		}
		for (unsigned j = 0; j < m; j++) {
			p[j][0] = tmp[j][0];
			p[j][1] = tmp[j][1];
		}
		n = m;
	}
	return n;
}

static float *emit(float *o, float x, float y, const float rgb[3], float alpha)
{
	o[0] = x * 2.0f - 1.0f;
	o[1] = 1.0f - y * 2.0f;
	o[2] = 0.0f;
	o[3] = 1.0f;
	o[4] = rgb[0];
	o[5] = rgb[1];
	o[6] = rgb[2];
	o[7] = alpha;
	return o + 8;
}

unsigned flatmesh_vertices_gpu_look(float *out, uint32_t t_ms, uint32_t center, uint32_t outer, float dx, float dy,
				    float alpha)
{
	static struct look l;
	l.dx = dx;
	l.dy = dy;
	animate(t_ms, look_vertex, &l);
	float cx = 0.5f + dx, cy = 0.5f + dy, inner = 0.5f * 0.99879546f;
	float *o = out;
	for (int t = 0; t < FLATMESH_INDICES / 3; t++) {
		float m = flatmesh_vertices[flatmesh_indices[3 * t]][2], rgb[3];
		for (int c = 0; c < 3; c++) {
			int s = 16 - 8 * c;
			rgb[c] = ((float)(center >> s & 0xff) * (1 - m) + (float)(outer >> s & 0xff) * m) / 255.0f * alpha;
		}
		float p[ROUND_SIDES + 3][2];
		bool inside = true;
		for (int k = 0; k < 3; k++) {
			p[k][0] = l.pos[3 * t + k][0];
			p[k][1] = l.pos[3 * t + k][1];
			float ex = p[k][0] - cx, ey = p[k][1] - cy;
			inside &= ex * ex + ey * ey <= inner * inner;
		}
		unsigned n = inside ? 3 : clip_round(p, 3, cx, cy);
		for (unsigned k = 1; k + 1 < n; k++) {
			o = emit(o, p[0][0], p[0][1], rgb, alpha);
			o = emit(o, p[k][0], p[k][1], rgb, alpha);
			o = emit(o, p[k + 1][0], p[k + 1][1], rgb, alpha);
		}
	}
	return (unsigned)((o - out) / 8);
}
