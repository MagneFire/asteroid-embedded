/* Which watch this is. The QCDT table in the boot image holds one
 * device tree per watch and each watch's bootloader passes the one whose
 * board-id it has, so the tree's compatible names the watch; SMEM's
 * board-id cannot (sturgeon and sparrow both read platform 8, subtype 4).
 * If the tree is missing, the size of the bootloader's splash, which is
 * the panel's, tells them apart.
 *
 * smelt's SMEM platform, the one its trees ask for, and its panel size go
 * first: its bootloader's way of choosing a tree is not known, and one
 * that falls back to another watch's tree must not get smelt the other
 * watch's charger setup. Without a splash, a tree is not proof either, and
 * smelt's fuel gauge is asked: this only happens with the display off, so
 * the other watches' boots stay as they are. With neither a tree nor a
 * splash the first table is a guess, and board_assumed keeps its charger
 * setup from running. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "lib/fdt.h"
#include "soc/max17050.h"

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

static bool smelt_gauge_answers(void)
{
	const struct board *s = &board_smelt;
	uint8_t reg = MAX17050_DEVNAME, id[2];
	if (i2c_init(s->gauge.i2c.bus, s->gauge.i2c.sda, s->gauge.i2c.scl, s->gauge.i2c.func, 200000))
		return false;
	return !i2c_xfer(s->gauge.i2c.bus, s->gauge.i2c.addr, &reg, 1, id, 2) && (id[0] | id[1] << 8) == MAX17050_DEVNAME_ID;
}

void board_detect(uint32_t machtype, const void *fdt)
{
	uint32_t w = 0, h = 0;
	const char *model = fdt_valid(fdt) ? fdt_prop(fdt, "/", "model", NULL) : NULL;
	uint32_t platform = smem_hw_platform();
	if (chip == &chip_8009) {
		board = &board_catfish;
		board_detected_by = "SoC";
	} else if (display_splash_size(&w, &h), platform == board_smelt.smem_platform) {
		board = &board_smelt;
		board_detected_by = "SMEM";
	} else if (by_panel(w, h) == &board_smelt) {
		board = &board_smelt;
		board_detected_by = "panel size";
	} else if (!w && smelt_gauge_answers()) {
		board = &board_smelt;
		board_detected_by = "fuel gauge";
	} else if (fdt_valid(fdt) && (board = by_compatible(fdt)))
		board_detected_by = "device tree";
	else if ((board = by_panel(w, h)))
		board_detected_by = "panel size";
	else {
		board = boards[0];
		board_detected_by = "nothing, assumed";
		board_assumed = true;
	}
	log_printf("board: %s by %s (tree %08lx %s, machtype %lu, panel %lux%lu, platform %lu)\n", board->name, board_detected_by,
		   (uint32_t)(uintptr_t)fdt, model ? model : "none", machtype, w, h, platform);
}
