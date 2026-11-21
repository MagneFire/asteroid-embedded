#include "input/tma525_proto.h"

#define REPORT_PIP	0x1f
#define CMD_SYSINFO	0x02
#define SYSINFO_LEN	48

const uint8_t tma525_get_sysinfo[7] = { 0x04, 0x00, 0x05, 0x00, 0x2f, 0x00, CMD_SYSINFO };

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

bool tma525_sysinfo_parse(const uint8_t *r, unsigned n, struct tma525_sysinfo *s)
{
	if (n < SYSINFO_LEN || le16(r) < SYSINFO_LEN || r[2] != REPORT_PIP || (r[4] & 0x7f) != CMD_SYSINFO)
		return false;
	s->revctrl = (uint32_t)r[11] | (uint32_t)r[12] << 8 | (uint32_t)r[13] << 16 | (uint32_t)r[14] << 24;
	s->electrodes_x = r[33];
	s->electrodes_y = r[34];
	s->res_x = le16(r + 39);
	s->res_y = le16(r + 41);
	s->panel_id = r[47];
	return s->res_x && s->res_y;
}

bool tma525_fits_panel(const struct tma525_sysinfo *s, uint16_t w, uint16_t h)
{
	return (s->res_x == h && s->res_y == w) || (s->res_x == w && s->res_y == h);
}

enum tma525_grid_from tma525_grid(const struct tma525_sysinfo *s, uint16_t board_w, uint16_t board_h, uint16_t panel_w,
				  uint16_t panel_h, uint16_t *w, uint16_t *h)
{
	if (board_w && board_h) {
		*w = board_w;
		*h = board_h;
		return TMA525_GRID_BOARD;
	}
	/* The panel's size either way round is taken as the panel's grid:
	 * which axis is which is the decoder's. */
	if (s && !tma525_fits_panel(s, panel_w, panel_h)) {
		*w = s->res_y;
		*h = s->res_x;
		return TMA525_GRID_SYSINFO;
	}
	*w = *h = 0;
	return TMA525_GRID_PANEL;
}
