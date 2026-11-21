/* The TMA525's system information, from the PIP command GET_SYSINFO: the
 * controller's resolution and electrode counts, in its own axes. Its x
 * is the panel's y, as the touch records are read. */
#ifndef TMA525_PROTO_H
#define TMA525_PROTO_H
#include <stdbool.h>
#include <stdint.h>

struct tma525_sysinfo {
	uint32_t revctrl;
	uint16_t res_x, res_y;
	uint8_t electrodes_x, electrodes_y, panel_id;
};

enum tma525_grid_from { TMA525_GRID_BOARD, TMA525_GRID_SYSINFO, TMA525_GRID_PANEL };

/* To the output register: its length, report 2F, command 02. */
extern const uint8_t tma525_get_sysinfo[7];
/* A report, its length first: true if it is the reply, then s set. */
bool tma525_sysinfo_parse(const uint8_t *r, unsigned n, struct tma525_sysinfo *s);
/* The resolution is the panel's, either way round. */
bool tma525_fits_panel(const struct tma525_sysinfo *s, uint16_t w, uint16_t h);
/* The grid to scale touches from to the panel: the board's when it has
 * one, else the controller's when it is not the panel's (s NULL for
 * none read), else 0 by 0 for the panel's. */
enum tma525_grid_from tma525_grid(const struct tma525_sysinfo *s, uint16_t board_w, uint16_t board_h, uint16_t panel_w,
				  uint16_t panel_h, uint16_t *w, uint16_t *h);
#endif
