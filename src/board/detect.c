/* Which watch this is. The QCDT table in the boot image holds one
 * device tree per watch and each watch's bootloader passes the one whose
 * board-id it has, so the tree's compatible names the watch; SMEM's
 * board-id cannot (sturgeon and sparrow both read platform 8, subtype 4).
 * If the tree is missing, the size of the bootloader's splash, which is
 * the panel's, tells them apart.
 *
 * smelt's panel size goes first: its bootloader's way of choosing a tree
 * is not known, and one that falls back to another watch's tree must not
 * get smelt the other watch's charger setup. With neither a tree nor a
 * splash the first table is a guess, and board_assumed keeps its charger
 * setup from running. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "lib/fdt.h"

static const struct board *const boards[] = { &board_sturgeon, &board_sparrow, &board_smelt };

const struct board *board;
const char *board_detected_by;
bool board_assumed;

static const struct board *by_compatible(const void *fdt)
{
	uint32_t len;
	const char *compat = fdt_prop(fdt, "/", "compatible", &len);
	for (unsigned i = 0; compat && i < sizeof boards / sizeof boards[0]; i++)
		if (fdt_strlist_has(compat, len, boards[i]->compatible))
			return boards[i];
	return NULL;
}

static const struct board *by_panel(uint32_t w, uint32_t h)
{
	for (unsigned i = 0; i < sizeof boards / sizeof boards[0]; i++)
		if (w == boards[i]->panel.width && (h == boards[i]->panel.height || h == boards[i]->panel.height_alt))
			return boards[i];
	return NULL;
}

void board_detect(uint32_t machtype, const void *fdt)
{
	uint32_t w, h;
	display_splash_size(&w, &h);
	const char *model = fdt_valid(fdt) ? fdt_prop(fdt, "/", "model", NULL) : NULL;
	if (by_panel(w, h) == &board_smelt) {
		board = &board_smelt;
		board_detected_by = "panel size";
	} else if (fdt_valid(fdt) && (board = by_compatible(fdt)))
		board_detected_by = "device tree";
	else if ((board = by_panel(w, h)))
		board_detected_by = "panel size";
	else {
		board = boards[0];
		board_detected_by = "nothing, assumed";
		board_assumed = true;
	}
	log_printf("board: %s by %s (tree %08lx %s, machtype %lu, panel %lux%lu)\n", board->name, board_detected_by,
		   (uint32_t)(uintptr_t)fdt, model ? model : "none", machtype, w, h);
}
