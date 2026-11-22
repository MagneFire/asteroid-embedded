/* What the Compass app shows of the heading: whole degrees from 0 to
 * 359 and an enum ui_compass_state from the field's accuracy. A new
 * state shows at once; the heading, once it is COMPASS_HOLD from the
 * one shown and at most every COMPASS_SHOW_MS, as each change redraws
 * most of the screen. */
#ifndef COMPASS_APP_H
#define COMPASS_APP_H
#include <stdbool.h>
#include <stdint.h>

#define COMPASS_HOLD	0.75f
#define COMPASS_SHOW_MS	100

struct compass_view {
	int32_t degrees, state;
	uint32_t shown_t;
};

void compass_view_reset(struct compass_view *v);
/* The heading in degrees clockwise from north (0-360) and its accuracy
 * (0-3, -1 for none) at now: whether what v shows changed. */
bool compass_view_update(struct compass_view *v, float degrees, int accuracy, uint32_t now);

/* The page: No data until a heading taken since it opened or the
 * watch woke, and the hub's compass on while on is. A page of the
 * strip opening, Compass or not, and the watch going to sleep turn
 * it off at once. */
void compass_app_page(bool compass);
void compass_app_sleep(void);
void compass_app_pass(bool on, uint32_t now);
#endif
