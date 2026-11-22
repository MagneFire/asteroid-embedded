/* The Cortex-M build's mesh: grey and black, or the open app's colours,
 * and all of it drawn again when they change; and the strip's dots. */
#include <stdio.h>
#include "display/flatmesh_mcu.h"
#include "ui/ui.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define W	402
#define H	476

static bool drew_whole;
bool ui_draw(uint32_t panel, bool whole)
{
	drew_whole = whole;
	return true;
}
void ui_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {}

/* The sums of red, green and blue over the middle line, in RGB565's
 * 5 bits each. */
static void sums(unsigned rgb[3])
{
	static uint16_t line[W];
	ui_line_under(H / 2, 0, W, line);
	rgb[0] = rgb[1] = rgb[2] = 0;
	for (int x = 0; x < W; x++) {
		rgb[0] += line[x] >> 11;
		rgb[1] += (line[x] >> 6) & 31;
		rgb[2] += line[x] & 31;
	}
}

/* Red, in 5 bits, at x on line y over black. */
static unsigned over(int y, int x)
{
	static uint16_t line[W];
	for (int i = 0; i < W; i++)
		line[i] = 0;
	ui_line_over((uint32_t)y, 0, W, line);
	return line[x] >> 11;
}

/* As PageDot: 12 px discs (3 % of 402) 15 px apart, their middles 21 px
 * up, the one on show white and the others at half; a drag that would
 * turn the page lights the page it would turn to instead. */
static void dots(void)
{
	struct backdrop b = { .screen = UI_PANEL_APP, .pages = 3, .page = 1, .next = -1 };
	int y = H - 21, x = W / 2;
	flatmesh_mcu_draw(UI_PANEL_APP, &b, true);
	CHECK(over(y, x) == 31);
	CHECK(over(y, x + 15) == 16 && over(y, x - 15) == 16);
	CHECK(over(y, x + 8) == 0 && over(y - 8, x) == 0);
	CHECK(over(y + 3, x + 3) == 31 && over(y - 5, x + 5) == 0);
	b.next = 2;
	flatmesh_mcu_draw(UI_PANEL_APP, &b, false);
	CHECK(over(y, x) == 16 && over(y, x + 15) == 31);
	b.pages = 0;
	flatmesh_mcu_draw(UI_PANEL_APP, &b, false);
	CHECK(over(y, x) == 0);
}

int main(void)
{
	flatmesh_mcu_init(W, H);
	struct backdrop b = { .screen = UI_PANEL_FACE };
	unsigned rgb[3];
	CHECK(flatmesh_mcu_draw(UI_PANEL_APP, &b, false) && drew_whole);
	sums(rgb);
	CHECK(rgb[0] > 0 && rgb[0] == rgb[1] && rgb[1] == rgb[2]);
	flatmesh_mcu_draw(UI_PANEL_APP, &b, false);
	CHECK(!drew_whole);
	/* asteroid-compass's #29A600 / #070C00. */
	static const struct ui_colors green = { { 0x29, 0xa6, 0x00 }, { 0x07, 0x0c, 0x00 } };
	b.colors = &green;
	flatmesh_mcu_draw(UI_PANEL_APP, &b, false);
	CHECK(drew_whole);
	sums(rgb);
	CHECK(rgb[1] > 3 * rgb[0] && rgb[2] == 0);
	flatmesh_mcu_draw(UI_PANEL_APP, &b, false);
	CHECK(!drew_whole);
	b.colors = NULL;
	flatmesh_mcu_draw(UI_PANEL_APP, &b, false);
	CHECK(drew_whole);
	sums(rgb);
	CHECK(rgb[0] == rgb[1] && rgb[1] == rgb[2]);
	dots();
	if (failures)
		return 1;
	printf("flatmesh_mcu_test: ok\n");
	return 0;
}
