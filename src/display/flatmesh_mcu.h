/* What the Cortex-M build draws under its one Slint window, line by line
 * (ui_line_under): one still frame of the FlatMesh, moved and dimmed as
 * the AP builds have it at rest on each screen, or black on the
 * always-on face. flatmesh_mcu_draw draws panel over that backdrop, as
 * ui_draw, all of it when the mesh changed. */
#ifndef FLATMESH_MCU_H
#define FLATMESH_MCU_H
#include <stdbool.h>
#include <stdint.h>

struct backdrop {
	/* The stack's screen, under the overlay while that is up. */
	unsigned screen;
	bool ambient;
};

void flatmesh_mcu_init(uint32_t width, uint32_t height);
bool flatmesh_mcu_draw(unsigned panel, const struct backdrop *b, bool whole);
#endif
