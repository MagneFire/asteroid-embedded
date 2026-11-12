/* Which watch this is. One Apollo3 watch so far, and nothing passed in
 * by the boot loader: the board is beluga, and the panel tells its two
 * sizes apart once the display driver has read its ID. */
#include "asteroid.h"

const struct board *board = &board_beluga;
const char *board_detected_by = "the only Apollo3 board";
bool board_assumed;

void board_detect(uint32_t machtype, const void *fdt)
{
	log_printf("board: %s (%s)\n", board->name, board_detected_by);
}
