/* The TMA525's GET_SYSINFO reply, laid out as the cyttsp5 driver reads
 * it, with the two OPPO Watch configurations; other reports and short
 * ones are not taken for it. Then the grid touches are scaled from. */
#include <stdio.h>
#include <string.h>
#include "input/tma525_proto.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static unsigned reply(uint8_t *r, uint16_t res_x, uint16_t res_y, uint8_t ex, uint8_t ey)
{
	unsigned n = 51;
	memset(r, 0, 64);
	r[0] = (uint8_t)n;
	r[2] = 0x1f;
	r[4] = 0x80 | 0x02;
	r[11] = 0x37, r[12] = 0x8b, r[13] = 0x0d;
	r[33] = ex, r[34] = ey;
	r[39] = (uint8_t)res_x, r[40] = (uint8_t)(res_x >> 8);
	r[41] = (uint8_t)res_y, r[42] = (uint8_t)(res_y >> 8);
	r[47] = 0x05;
	return n;
}

int main(void)
{
	uint8_t r[64];
	struct tma525_sysinfo s;

	CHECK(sizeof tma525_get_sysinfo == 7 && tma525_get_sysinfo[4] == 0x2f && tma525_get_sysinfo[6] == 0x02);

	/* 46 mm: DC 01 92 01 in the touch firmware's configuration. */
	unsigned n = reply(r, 476, 402, 8, 9);
	CHECK(tma525_sysinfo_parse(r, n, &s));
	CHECK(s.res_x == 476 && s.res_y == 402 && s.electrodes_x == 8 && s.electrodes_y == 9);
	CHECK(s.panel_id == 0x05 && s.revctrl == 0x000d8b37);
	CHECK(tma525_fits_panel(&s, 402, 476) && !tma525_fits_panel(&s, 320, 360));
	/* 41 mm: 40 01 68 01. */
	n = reply(r, 320, 360, 7, 7);
	CHECK(tma525_sysinfo_parse(r, n, &s));
	CHECK(s.res_x == 320 && s.res_y == 360);
	CHECK(tma525_fits_panel(&s, 320, 360) && !tma525_fits_panel(&s, 402, 476));

	/* The echoed command without its toggle bit. */
	r[4] = 0x02;
	CHECK(tma525_sysinfo_parse(r, n, &s));
	/* Not the reply: a touch report, another command, too short. */
	r[2] = 0x01;
	CHECK(!tma525_sysinfo_parse(r, n, &s));
	n = reply(r, 320, 360, 7, 7);
	r[4] = 0x80 | 0x1b;
	CHECK(!tma525_sysinfo_parse(r, n, &s));
	n = reply(r, 320, 360, 7, 7);
	CHECK(!tma525_sysinfo_parse(r, 40, &s));
	r[0] = 40;
	CHECK(!tma525_sysinfo_parse(r, n, &s));
	n = reply(r, 0, 0, 7, 7);
	CHECK(!tma525_sysinfo_parse(r, n, &s));

	uint16_t w, h;
	/* The 46 mm board's own grid wins over what the controller says. */
	s = (struct tma525_sysinfo){ .res_x = 476, .res_y = 402 };
	CHECK(tma525_grid(&s, 368, 448, 402, 476, &w, &h) == TMA525_GRID_BOARD && w == 368 && h == 448);
	CHECK(tma525_grid(NULL, 368, 448, 402, 476, &w, &h) == TMA525_GRID_BOARD && w == 368 && h == 448);
	/* No board grid: the panel's size either way round is the panel. */
	CHECK(tma525_grid(&s, 0, 0, 402, 476, &w, &h) == TMA525_GRID_PANEL && !w && !h);
	s = (struct tma525_sysinfo){ .res_x = 320, .res_y = 360 };
	CHECK(tma525_grid(&s, 0, 0, 320, 360, &w, &h) == TMA525_GRID_PANEL && !w && !h);
	CHECK(tma525_grid(NULL, 0, 0, 320, 360, &w, &h) == TMA525_GRID_PANEL && !w && !h);
	/* Another grid, in the decoder's axes: the controller's y is x. */
	s = (struct tma525_sysinfo){ .res_x = 448, .res_y = 368 };
	CHECK(tma525_grid(&s, 0, 0, 320, 360, &w, &h) == TMA525_GRID_SYSINFO && w == 368 && h == 448);

	if (failures)
		printf("tma525_test: %d failures\n", failures);
	else
		printf("tma525_test: ok\n");
	return failures != 0;
}
