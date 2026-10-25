/* Text console on the framebuffer: a 5x12 font drawn at 2x, a character
 * grid kept in RAM so a scroll is a full redraw, and the margins keep the
 * text inside the round panel. */
#include <string.h>
#include "asteroid.h"
#include "display.h"
#include "font5x12.h"

#define FONT_W 5
#define FONT_H 12
#define SCALE 2
#define CELL_W (FONT_W * SCALE + 2)
#define CELL_H (FONT_H * SCALE + 2)
#define MAX_COLS 64
#define MAX_ROWS 32
#define FG 0x00ffffffu
#define BG 0x00000000u

static char grid[MAX_ROWS][MAX_COLS];
static unsigned cols, rows, x0, y0, cx, cy;
static bool ready;

static void put_pixel(unsigned x, unsigned y, uint32_t c)
{
	uint8_t *p = (uint8_t *)fb.base + (y * fb.stride + x) * fb.bpp;
	if (fb.bpp == 4)
		*(uint32_t *)p = c;
	else if (fb.bpp == 3) {
		p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)(c >> 16);
	} else
		*(uint16_t *)p = (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
}

static void draw_glyph(unsigned col, unsigned row, char ch)
{
	unsigned px = x0 + col * CELL_W, py = y0 + row * CELL_H;
	const unsigned *g = font5x12 + 2 * ((unsigned char)ch < 32 || (unsigned char)ch > 127 ? 0 : (unsigned char)ch - 32);
	for (unsigned y = 0; y < FONT_H; y++) {
		unsigned bits = g[y / 6] >> (FONT_W * (y % 6));
		for (unsigned x = 0; x < FONT_W; x++) {
			uint32_t c = (bits >> x) & 1 ? FG : BG;
			for (unsigned sy = 0; sy < SCALE; sy++)
				for (unsigned sx = 0; sx < SCALE; sx++)
					put_pixel(px + x * SCALE + sx, py + y * SCALE + sy, c);
		}
	}
}

static void redraw(void)
{
	for (unsigned r = 0; r < rows; r++)
		for (unsigned c = 0; c < cols; c++)
			draw_glyph(c, r, grid[r][c] ? grid[r][c] : ' ');
}

void fbcon_init(void)
{
	if (!fb.base)
		return;
	unsigned side = fb.width < fb.height ? fb.width : fb.height;
	unsigned box = side * 70 / 100;
	cols = box / CELL_W;
	rows = box / CELL_H;
	if (cols > MAX_COLS) cols = MAX_COLS;
	if (rows > MAX_ROWS) rows = MAX_ROWS;
	x0 = (fb.width - cols * CELL_W) / 2;
	y0 = (fb.height - rows * CELL_H) / 2;
	for (unsigned y = 0; y < fb.height; y++)
		for (unsigned x = 0; x < fb.width; x++)
			put_pixel(x, y, BG);
	memset(grid, 0, sizeof grid);
	cx = cy = 0;
	ready = true;
	display_update();
}

static void newline(void)
{
	cx = 0;
	if (cy + 1 < rows) {
		cy++;
		return;
	}
	memmove(grid[0], grid[1], sizeof grid[0] * (rows - 1));
	memset(grid[rows - 1], 0, sizeof grid[0]);
	redraw();
}

void fbcon_puts(const char *s)
{
	if (!ready)
		return;
	for (; *s; s++) {
		if (*s == '\n') {
			newline();
			continue;
		}
		if (cx >= cols)
			newline();
		grid[cy][cx] = *s;
		draw_glyph(cx, cy, *s);
		cx++;
	}
	display_update();
}

void fbcon_printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	fbcon_puts(buf);
}
