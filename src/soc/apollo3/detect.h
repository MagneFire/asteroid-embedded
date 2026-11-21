#ifndef APOLLO3_DETECT_H
#define APOLLO3_DETECT_H
#include "display/rm69090_id.h"

struct board;
/* For a boot where the AP has the panel: the size the last boot that
 * read it found, kept across the reset. False, and the 46 mm watch,
 * without one. */
bool board_detect_kept(void);
/* The size whose panel ID the replies to DA, DB and DC give, and how
 * they gave it; NULL for neither. */
const struct board *board_by_panel_id(const uint8_t raw[3][2], enum rm69090_framing *f);
/* Takes that size, or the 46 mm watch's for neither or for raw NULL (the
 * panel not read), logs the choice and gives the framing. */
enum rm69090_framing board_detect_panel(const uint8_t raw[3][2]);
#endif
