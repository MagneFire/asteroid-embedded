/* Who has the hands of a watch with hands over its screen: the time,
 * a pose that keeps them off what is on show, or the calibration page.
 * handpose_want picks that from what is on show, and handpose_step says
 * when to move them: at once to the time and for calibration, to a pose
 * only once something has wanted them off where they are for
 * HANDPOSE_SETTLE_MS. Positions are steps of 2 degrees clockwise from 12. */
#ifndef HANDPOSE_H
#define HANDPOSE_H
#include <stdint.h>
#include <stdbool.h>

#define HANDPOSE_SETTLE_MS 500

enum handpose_owner { HANDS_TIME, HANDS_POSE, HANDS_CALIBRATE };

struct handpose {
	uint8_t owner, minute, hour;
};

/* What is on show: the stack's panel, the app open on the app panel and
 * whether a pop-up is over them. */
struct handpose_view {
	bool awake, calibrate, modal;
	unsigned panel;
	int app;
};

struct handpose_state {
	struct handpose cur;
	bool waiting;
	uint32_t since;
};

struct handpose handpose_want(const struct handpose_view *v, struct handpose cur);
/* True when the hands are to go to s->cur now. */
bool handpose_step(struct handpose_state *s, struct handpose want, uint32_t now);
uint32_t handpose_next_ms(const struct handpose_state *s, uint32_t now);
#endif
