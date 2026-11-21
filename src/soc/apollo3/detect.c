/* Which watch this is. One Apollo3 watch so far, and nothing passed in
 * by the boot loader: the board is beluga, and the panel tells its two
 * sizes apart once the display driver has read its ID. Until then, and
 * when it cannot be read, it is the 46 mm watch. The size found is kept
 * across a warm reset for a boot where the AP has the panel. */
#include "asteroid.h"
#include "detect.h"

#ifdef HOST_TEST
#define KEPT
#else
#define KEPT		__attribute__((section(".kept")))
#endif
#define KEPT_MAGIC	0x53495a31u	/* "SIZ1" */

static const struct board *const sizes[] = { &board_beluga, &board_beluga_41 };
static struct {
	uint32_t magic;
	uint16_t size, check;
} kept KEPT;

const struct board *board = &board_beluga;
const char *board_detected_by = "nothing yet";
bool board_assumed;

void board_detect(uint32_t machtype, const void *fdt)
{
	log_printf("board: %s until the panel ID is read\n", board->name);
}

bool board_detect_kept(void)
{
	bool known = kept.magic == KEPT_MAGIC && kept.size < ARRAY_SIZE(sizes) && kept.check == (uint16_t)~kept.size;
	board = known ? sizes[kept.size] : &board_beluga;
	board_detected_by = known ? "the last boot that read the panel" : "nothing, panel not read";
	log_printf("board: %s by %s\n", board->name, board_detected_by);
	return known;
}

const struct board *board_by_panel_id(const uint8_t raw[3][2], enum rm69090_framing *f)
{
	for (unsigned i = 0; raw && i < ARRAY_SIZE(sizes); i++) {
		*f = rm69090_match(raw, sizes[i]->panel.id);
		if (*f != RM69090_UNKNOWN)
			return sizes[i];
	}
	*f = RM69090_UNKNOWN;
	return NULL;
}

enum rm69090_framing board_detect_panel(const uint8_t raw[3][2])
{
	enum rm69090_framing f;
	const struct board *b = board_by_panel_id(raw, &f);
	board = b ? b : &board_beluga;
	board_detected_by = b ? "panel ID" : raw ? "nothing, unknown panel ID" : "nothing, panel not read";
	if (raw) {
		kept.size = board == &board_beluga_41;
		kept.check = (uint16_t)~kept.size;
		kept.magic = KEPT_MAGIC;
		const uint8_t *id = b ? b->panel.id : (const uint8_t[]){ raw[0][0], raw[1][0], raw[2][0] };
		log_printf("board: %s by %s, id %02x %02x %02x %s\n", board->name, board_detected_by, id[0], id[1], id[2],
			   rm69090_framing_name(f));
	} else
		log_printf("board: %s by %s\n", board->name, board_detected_by);
	return f;
}

const struct board *board_by_touch(const struct tma525_sysinfo *s)
{
	for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++)
		if (tma525_fits_panel(s, sizes[i]->panel.width, sizes[i]->panel.height))
			return sizes[i];
	return NULL;
}

void board_check_touch(const struct tma525_sysinfo *s)
{
	const struct board *b = board_by_touch(s);
	if (b == board)
		log_printf("board: touch %ux%u agrees with %s\n", s->res_x, s->res_y, board->name);
	else if (b)
		log_printf("board: warning: touch %ux%u is %s's, keeping %s by %s\n", s->res_x, s->res_y, b->name, board->name,
			   board_detected_by);
	else
		log_printf("board: touch %ux%u fits neither size, keeping %s\n", s->res_x, s->res_y, board->name);
}
