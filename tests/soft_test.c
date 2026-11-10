/* The CPU's frame drawing on the host: triangles that share edges
 * cover every pixel once, and a layer blended over the frame matches
 * the arithmetic done a pixel at a time. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asteroid.h"
#include "gpu/draw.h"

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

int main(void)
{
	gpu_host_base = (uintptr_t)&mem;
	triangles();
	layers();
	round_window();
	printf("soft_test: ok\n");
	return 0;
}
