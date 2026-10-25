/* Text console: a 5x12 font drawn at 2x from a character grid that keeps
 * the text inside the round panel. In console mode every write redraws
 * the grid into the back buffer and flips; as an overlay the grid is drawn
 * over whatever the frame loop rendered. */
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
static bool ready, overlay;

static void draw_glyph(uint32_t *buf, unsigned col, unsigned row, char ch, bool bg)
{
	unsigned px = x0 + col * CELL_W, py = y0 + row * CELL_H;
	const unsigned *g = font5x12 + 2 * ((unsigned char)ch < 32 || (unsigned char)ch > 127 ? 0 : (unsigned char)ch - 32);
	for (unsigned y = 0; y < FONT_H; y++) {
		unsigned bits = g[y / 6] >> (FONT_W * (y % 6));
		for (unsigned x = 0; x < FONT_W; x++) {
			bool on = (bits >> x) & 1;
			if (!on && !bg)
				continue;
			uint32_t c = on ? FG : BG;
			for (unsigned sy = 0; sy < SCALE; sy++) {
				uint32_t *p = buf + (py + y * SCALE + sy) * fb.stride + px + x * SCALE;
				for (unsigned sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
	}
}

void fbcon_draw(uint32_t *buf)
{
	if (!ready)
		return;
	for (unsigned r = 0; r < rows; r++)
		for (unsigned c = 0; c < cols; c++)
			if (grid[r][c] && grid[r][c] != ' ')
				draw_glyph(buf, c, r, grid[r][c], !overlay);
}

static void present(void)
{
	uint32_t *buf = display_backbuffer();
	memset(buf, 0, fb.stride * fb.height * 4);
	fbcon_draw(buf);
	display_flip();
}

void fbcon_init(void)
{
	if (!fb.buf[0])
		return;
	unsigned side = fb.width < fb.height ? fb.width : fb.height;
	unsigned box = side * 80 / 100;
	cols = box / CELL_W;
	rows = box / CELL_H;
	if (cols > MAX_COLS) cols = MAX_COLS;
	if (rows > MAX_ROWS) rows = MAX_ROWS;
	x0 = (fb.width - cols * CELL_W) / 2;
	y0 = (fb.height - rows * CELL_H) / 2;
	memset(grid, 0, sizeof grid);
	cx = cy = 0;
	ready = true;
	present();
}

void fbcon_set_overlay(bool on) { overlay = on; }

static void newline(void)
{
	cx = 0;
	if (cy + 1 < rows) {
		cy++;
		return;
	}
	memmove(grid[0], grid[1], sizeof grid[0] * (rows - 1));
	memset(grid[rows - 1], 0, sizeof grid[0]);
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
		grid[cy][cx++] = *s;
	}
	if (!overlay)
		present();
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
