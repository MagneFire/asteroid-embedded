/* The CPU's frame drawing on the host: triangles that share edges
 * cover every pixel once, and a layer blended over the frame matches
 * the arithmetic done a pixel at a time. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asteroid.h"
#include "gpu/draw.h"
#include "gpu/paint.h"

void rotation_buffer(uint32_t *buf, uint32_t stride) {}

#define W 96
#define H 80
#define GRID 9

/* One block for everything the drawing calls are given, so that it can
 * be named by 32-bit offsets from gpu_host_base. */
static struct {
	uint32_t frame[H * W], layer[H * W];
	float vbo[(GRID - 1) * (GRID - 1) * 6 * 8];
} mem;
#define frame	mem.frame
#define layer	mem.layer
#define vbo	mem.vbo
uintptr_t gpu_host_base;
#define ADDR(p)	((uint32_t)((uintptr_t)(p) - gpu_host_base))

static uint32_t rnd(void)
{
	static uint32_t s = 12345;
	s = s * 1664525u + 1013904223u;
	return s >> 8;
}

static uint32_t div255(uint32_t v)
{
	v += 128;
	return (v + (v >> 8)) >> 8;
}

static void check(bool ok, const char *what)
{
	if (!ok) {
		printf("soft_test: %s\n", what);
		exit(1);
	}
}

/* A grid of points, the inner ones moved off the grid, two triangles a
 * cell: together they cover the frame exactly. Blended at half, a
 * pixel drawn twice or not at all shows in its value. */
static void triangles(void)
{
	static float px[GRID][GRID], py[GRID][GRID];
	for (int j = 0; j < GRID; j++)
		for (int i = 0; i < GRID; i++) {
			bool edge = !i || !j || i == GRID - 1 || j == GRID - 1;
			px[j][i] = (float)i / (GRID - 1) * 2 - 1 + (edge ? 0 : ((float)(rnd() % 100) - 50) / 600);
			py[j][i] = (float)j / (GRID - 1) * 2 - 1 + (edge ? 0 : ((float)(rnd() % 100) - 50) / 600);
		}
	unsigned n = 0;
	for (int j = 0; j + 1 < GRID; j++)
		for (int i = 0; i + 1 < GRID; i++) {
			const int c[6][2] = { { i, j }, { i + 1, j }, { i + 1, j + 1 }, { i, j }, { i + 1, j + 1 }, { i, j + 1 } };
			for (int k = 0; k < 6; k++) {
				float *o = vbo + n++ * 8;
				o[0] = px[c[k][1]][c[k][0]];
				o[1] = py[c[k][1]][c[k][0]];
				o[2] = 0;
				o[3] = 1;
				o[4] = o[5] = o[6] = 0.25f;
				o[7] = 0.5f;
			}
		}
	for (int i = 0; i < H * W; i++)
		frame[i] = 0x808080;
	struct gpu_target t = { ADDR(frame), W, H, W * 4, false };
	gpu_draw_begin(&t);
	gpu_draw_blend(true);
	gpu_draw_triangles(ADDR(vbo), 32, 2, (const uint8_t[]){ 4, 4 }, n);
	uint32_t once = 64 + div255(0x80 * 127);
	for (int i = 0; i < H * W; i++)
		check((frame[i] & 0xff) == once && (frame[i] >> 8 & 0xff) == once && (frame[i] >> 16 & 0xff) == once,
		      "a pixel of the mesh was not drawn exactly once");
	gpu_draw_blend(false);
	gpu_draw_triangles(ADDR(vbo), 32, 2, (const uint8_t[]){ 4, 4 }, n);
	for (int i = 0; i < H * W; i++)
		check(frame[i] == 0x404040, "an opaque mesh left a pixel");
}

static uint32_t over_ref(uint32_t d, uint32_t s, uint32_t fade)
{
	uint32_t c[4] = { s & 0xff, s >> 8 & 0xff, s >> 16 & 0xff, s >> 24 };
	if (!c[3])
		return d;
	if (fade < 256)
		for (int k = 0; k < 4; k++)
			c[k] = div255(c[k] * fade);
	uint32_t keep = 255 - c[3], o = 0;
	for (int k = 0; k < 3; k++) {
		uint32_t v = div255((d >> (16 - 8 * k) & 0xff) * keep) + c[k];
		o |= (v > 255 ? 255 : v) << (16 - 8 * k);
	}
	return o;
}

/* Premultiplied pixels, with runs of nothing and of opaque ones, at
 * every offset the eight-pixel loop can start and end on. */
static void layers(void)
{
	static uint32_t before[H * W];
	struct gpu_target t = { ADDR(frame), W, H, W * 4, false };
	for (int round = 0; round < 40; round++) {
		for (int i = 0; i < H * W; i++) {
			uint32_t a = rnd() % 4 == 0 ? 0 : rnd() % 4 == 0 ? 255 : rnd() & 0xff;
			if ((i / 16) % 5 == 0)
				a = 0;
			layer[i] = a << 24 | div255((rnd() & 0xff) * a) << 16 | div255((rnd() & 0xff) * a) << 8 |
				   div255((rnd() & 0xff) * a);
			before[i] = frame[i] = rnd() & 0xffffff;
		}
		int x = (int)(rnd() % 61) - 30, y = (int)(rnd() % 41) - 20;
		float alpha = round % 3 == 0 ? 1.0f : (float)(rnd() % 256) / 255.0f;
		uint32_t fade = alpha >= 1.0f ? 256 : (uint32_t)(alpha * 255.0f + 0.5f);
		struct gpu_rect part = { (int)(rnd() % 20), (int)(rnd() % 20), 30 + rnd() % 40, 20 + rnd() % 30 };
		gpu_draw_begin(&t);
		gpu_draw_layer_parts(ADDR(layer), W, H, W * 4, x, y, alpha, &part, 1);
		for (int ty = 0; ty < H; ty++)
			for (int tx = 0; tx < W; tx++) {
				int lx = tx - x, ly = ty - y;
				bool in = lx >= part.x && lx < part.x + (int)part.width && lx < W && ly >= part.y &&
					  ly < part.y + (int)part.height && ly < H && fade;
				uint32_t want = in ? over_ref(before[ty * W + tx], layer[ly * W + lx], fade) : before[ty * W + tx];
				check((frame[ty * W + tx] & 0xffffff) == want, "a blended pixel differs");
			}
	}
}

/* The round window: the same rows on both sides of the middle, never
 * wider than the layer, and the full width across the middle. */
static void round_window(void)
{
	struct gpu_target t = { ADDR(frame), H, H, W * 4, false };
	for (int i = 0; i < H * W; i++) {
		layer[i] = 0xffffffff;
		frame[i] = 0;
	}
	gpu_draw_begin(&t);
	gpu_draw_layer_round(ADDR(layer), H, H, W * 4, 0, 0, 1.0f);
	for (int y = 0; y < H; y++) {
		int n = 0, m = 0;
		for (int x = 0; x < H; x++) {
			n += (frame[y * W + x] & 0xffffff) == 0xffffff;
			m += (frame[(H - 1 - y) * W + x] & 0xffffff) == 0xffffff;
			check(frame[y * W + x] == frame[y * W + (H - 1 - x)], "the round window is not symmetric");
		}
		check(n == m, "the round window differs top to bottom");
		check(y != H / 2 || n == H, "the round window is not full across the middle");
		check(y != 0 || (n > 0 && n < H / 3), "the round window's top row is off");
	}
}

/* A texture drawn into a layer matches the arithmetic done a pixel at
 * a time, in every format, tinted and not, and stays inside its clip. */
static void textures(void)
{
	enum { TW = 37, TH = 5 };
	static uint8_t tex[TH * TW * 4];
	static const int formats[] = { PAINT_ALPHA, PAINT_RGBA_PREMULTIPLIED, PAINT_RGBA, PAINT_RGB };
	static const uint8_t sizes[] = { 1, 4, 4, 3 };
	for (unsigned f = 0; f < 4; f++)
		for (unsigned round = 0; round < 3; round++) {
			bool colorize = round == 2 && f != 0;
			uint32_t tint = f == 0 || colorize ? 0x80402010u + (round << 4) : round ? 0x99999999u : 0xffffffffu;
			unsigned bytes = sizes[f];
			for (unsigned i = 0; i < sizeof tex; i++)
				tex[i] = (uint8_t)(rnd() % 5 == 0 ? 0 : rnd());
			/* Premultiplied texels have no colour above their alpha. */
			if (formats[f] == PAINT_RGBA_PREMULTIPLIED)
				for (unsigned i = 0; i < TH * TW; i++)
					for (int c = 0; c < 3; c++)
						if (tex[i * 4 + c] > tex[i * 4 + 3])
							tex[i * 4 + c] = tex[i * 4 + 3];
			uint32_t want[H * W];
			for (int i = 0; i < H * W; i++) {
				uint32_t a = rnd() & 0xff;
				layer[i] = a << 24 | div255((rnd() & 0xff) * a) << 16 | div255((rnd() & 0xff) * a) << 8 |
					   div255((rnd() & 0xff) * a);
				want[i] = layer[i];
			}
			const int32_t clip[4] = { 5, 2, 30, 6 };
			int dx = 3, dy = 1;
			for (int y = clip[1]; y < clip[3] && y < dy + TH; y++)
				for (int x = clip[0]; x < clip[2] && x < dx + TW; x++) {
					const uint8_t *q = tex + ((y - dy) * TW + (x - dx)) * bytes;
					uint32_t p[4], t[4] = { tint & 0xff, tint >> 8 & 0xff, tint >> 16 & 0xff, tint >> 24 };
					if (f == 0 || colorize) {
						uint32_t a = bytes == 1 ? q[0] : bytes == 3 ? 255 : q[3];
						for (int c = 0; c < 4; c++)
							p[c] = div255(a * t[c]);
					} else {
						p[3] = bytes == 3 ? 255 : q[3];
						for (int c = 0; c < 3; c++)
							p[c] = formats[f] == PAINT_RGBA ? div255(q[c] * p[3]) : q[c];
						if (tint != 0xffffffffu)
							for (int c = 0; c < 4; c++)
								p[c] = div255(p[c] * t[c]);
					}
					uint8_t *d = (uint8_t *)&want[y * W + x];
					for (int c = 0; c < 4; c++) {
						uint32_t v = p[c] + div255(d[c] * (255 - p[3]));
						d[c] = (uint8_t)(v > 255 ? 255 : v);
					}
				}
			check(paint_begin(layer, W, H, W), "the layer is refused");
			check(paint_texture(tex, TW * bytes, TW, TH, formats[f], dx, dy, TW, TH, clip, tint, colorize),
			      "a texture at its own size is refused");
			check(!memcmp(layer, want, sizeof want), "a texture is not drawn as a pixel at a time would");
			check(f != 0 || !paint_texture(tex, TW, TW, TH, PAINT_ALPHA, dx, dy, TW * 2, TH, clip, tint, false),
			      "scaled coverage is taken");
		}
	/* Rectangles: copied, opaque and blended, inside the clip only. */
	for (unsigned round = 0; round < 3; round++) {
		uint32_t color = round == 0 ? 0x40201008u : round == 1 ? 0xff102030u : 0x80402010u, want[H * W];
		for (int i = 0; i < H * W; i++)
			want[i] = layer[i] = 0xff000000u | (rnd() & 0xffffff);
		const int32_t clip[4] = { 4, 3, 50, 9 };
		for (int y = 3; y < 9; y++)
			for (int x = 7; x < 50; x++) {
				uint8_t *d = (uint8_t *)&want[y * W + x], *c = (uint8_t *)&color;
				for (int k = 0; k < 4; k++) {
					uint32_t v = round == 0 ? c[k] : c[k] + div255(d[k] * (255 - c[3]));
					d[k] = (uint8_t)(v > 255 ? 255 : v);
				}
			}
		paint_rect(7.2f, 0.0f, 60.4f, 20.0f, clip, color, round == 0);
		check(!memcmp(layer, want, sizeof want), "a rectangle is not filled as a pixel at a time would");
	}
}

/* A scaled texture takes each pixel from the texel under it, and a
 * rounded rectangle with a radius of half its side is a disc: the same
 * left to right and top to bottom, of the right area, soft at its edge. */
static void scaled_and_rounded(void)
{
	static uint8_t tex[4 * 4 * 4];
	for (unsigned i = 0; i < 16; i++) {
		uint32_t c = 0xff000000u | (i * 16) << 16 | (i * 8) << 8 | (255 - i * 10);
		memcpy(tex + i * 4, &c, 4);
	}
	memset(layer, 0, sizeof(uint32_t) * H * W);
	const int32_t all[4] = { 0, 0, W, H };
	paint_begin(layer, W, H, W);
	check(paint_texture(tex, 16, 4, 4, PAINT_RGBA_PREMULTIPLIED, 10, 20, 40, 12, all, 0xffffffffu, false),
	      "a scaled picture is refused");
	for (int y = 0; y < 12; y++)
		for (int x = 0; x < 40; x++) {
			uint32_t want;
			memcpy(&want, tex + ((y / 3) * 4 + x / 10) * 4, 4);
			check(layer[(20 + y) * W + 10 + x] == want, "a scaled picture takes the wrong texel");
		}
	check(!layer[19 * W + 10] && !layer[32 * W + 10] && !layer[20 * W + 9] && !layer[20 * W + 50],
	      "a scaled picture goes past its place");

	enum { D = 60 };
	memset(layer, 0, sizeof(uint32_t) * H * W);
	const float radii[4] = { D / 2, D / 2, D / 2, D / 2 };
	check(paint_rounded(8, 6, 8 + D, 6 + D, radii, 0, all, 0xffffffffu, 0xffffffffu), "a rounded rectangle is refused");
	check(!paint_rounded(8, 6, 8 + D, 6 + D, radii, 2, all, 0xffffffffu, 0xff0000ffu), "a border is taken");
	uint32_t area = 0, soft = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			uint32_t a = layer[y * W + x] >> 24;
			bool in = x >= 8 && x < 8 + D && y >= 6 && y < 6 + D;
			check(in || !a, "a rounded rectangle goes past its place");
			if (!in)
				continue;
			area += a;
			soft += a > 0 && a < 255;
			check(a == layer[y * W + (8 + 8 + D - 1 - x)] >> 24, "a disc differs left to right");
			check(a == layer[(6 + 6 + D - 1 - y) * W + x] >> 24, "a disc differs top to bottom");
		}
	/* pi * 30 * 30 = 2827 pixels. */
	check(area > 2800 * 255 && area < 2855 * 255, "a disc has the wrong area");
	check(soft > 100 && soft < 400, "a disc's edge is not soft");
	check(layer[(6 + D / 2) * W + 8] >> 24 >= 250 && layer[6 * W + 8 + D / 2] >> 24 >= 250, "a disc does not reach its sides");
}

/* What is drawn under a rounded clip stays inside it and is drawn once:
 * a half-transparent rectangle over nothing is its own colour inside. */
static void rounded_clip(void)
{
	enum { D = 40 };
	memset(layer, 0, sizeof(uint32_t) * H * W);
	const int32_t all[4] = { 0, 0, W, H };
	const float clip[5] = { 10, 8, 10 + D, 8 + D, D / 2 };
	paint_begin(layer, W, H, W);
	paint_rounded_clip(clip);
	paint_rect(0, 0, W, H, all, 0x80808080u, false);
	paint_rounded_clip(NULL);
	uint32_t area = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			uint32_t p = layer[y * W + x], a = p >> 24;
			bool in = x >= 10 && x < 10 + D && y >= 8 && y < 8 + D;
			check(in || !p, "a rounded clip lets something past");
			check(a <= 0x80, "a pixel under a rounded clip is drawn twice");
			area += a;
		}
	/* pi * 20 * 20 = 1257 pixels at 128. */
	check(area > 1240 * 128 && area < 1275 * 128, "a rounded clip has the wrong area");
	check(layer[(8 + D / 2) * W + 10 + D / 2] == 0x80808080u, "the middle of a rounded clip is not drawn");
}

/* A picture half way through a quarter turn: its centre stays, and a
 * mark at the top is found an eighth of a turn clockwise. */
static void turned(void)
{
	enum { S = 64 };
	memset(frame, 0, sizeof(uint32_t) * H * W);
	for (int y = 4; y < 8; y++)
		for (int x = S / 2 - 2; x < S / 2 + 2; x++)
			frame[y * W + x] = 0xffffff;
	frame[(S / 2) * W + S / 2] = 0x00ff00;
	struct gpu_target t = { ADDR(frame), S, S, W * 4, false };
	gpu_draw_begin(&t);
	gpu_draw_turn(0.70710678f, 0.70710678f);
	gpu_draw_end();
	gpu_draw_turn(1.0f, 0.0f);
	int found = 0, fx = 0, fy = 0;
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++)
			if (frame[y * W + x] == 0xffffff) {
				found++;
				fx += x;
				fy += y;
			}
	check(found >= 10 && found <= 22, "the mark is lost in the turn");
	fx /= found;
	fy /= found;
	/* 26 from the centre, up and to the right: 26 / sqrt 2 = 18. */
	check(fx >= S / 2 + 16 && fx <= S / 2 + 20 && fy >= S / 2 - 21 && fy <= S / 2 - 16, "the mark is turned to the wrong place");
	bool centre = false;
	for (int y = S / 2 - 1; y <= S / 2 + 1; y++)
		for (int x = S / 2 - 1; x <= S / 2 + 1; x++)
			centre |= frame[y * W + x] == 0x00ff00;
	check(centre, "the centre moves in the turn");
}

int main(void)
{
	gpu_host_base = (uintptr_t)&mem;
	triangles();
	layers();
	round_window();
	textures();
	scaled_and_rounded();
	rounded_clip();
	turned();
	printf("soft_test: ok\n");
	return 0;
}
