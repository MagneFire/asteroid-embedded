/* What the Cortex-M build draws around its one Slint window, line by
 * line (ui_line_under and ui_line_over): under it one still frame of the
 * FlatMesh, moved and dimmed as the AP builds have it at rest on each
 * screen, or black on the always-on face; over it the stack's edge
 * indicators at rest, the left one out while notifications wait.
 * flatmesh_mcu_draw draws panel with that backdrop, as ui_draw: all of
 * it when the mesh changed, the indicators where they changed. */
#ifndef FLATMESH_MCU_H
#define FLATMESH_MCU_H
#include <stdbool.h>
#include <stdint.h>

struct backdrop {
	/* The stack's screen, under the overlay while that is up. */
	unsigned screen;
	bool ambient, modal;
	/* Edges with an indicator: left, right, top, bottom as bits 0-3. */
	unsigned edges;
	bool pending;
	/* Where the always-on face moved the waiting-notification
	 * indicator to. */
	int dx, dy;
};

void flatmesh_mcu_init(uint32_t width, uint32_t height);
bool flatmesh_mcu_draw(unsigned panel, const struct backdrop *b, bool whole);
#endif
