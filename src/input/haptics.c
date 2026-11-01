/* Vibration patterns: a pattern is a list of on/off durations, played
 * from the main loop on the watch's motor driver (board->motor). */
#include "asteroid.h"

static bool ready;
static const uint16_t *pattern;
static unsigned steps, step;
static uint32_t step_t;

bool haptics_init(void)
{
	ready = board->motor && board->motor->init();
	return ready;
}

void haptics_play(const uint16_t *on_off_ms, unsigned n)
{
	if (!ready)
		return;
	if (pattern)
		board->motor->set(false);
	pattern = n ? on_off_ms : NULL;
	steps = n;
	step = 0;
	step_t = time_ms();
	if (pattern)
		board->motor->set(true);
}

void haptics_poll(void)
{
	if (!pattern || time_ms() - step_t < pattern[step])
		return;
	step_t = time_ms();
	if (++step == steps) {
		pattern = NULL;
		board->motor->set(false);
		return;
	}
	board->motor->set(!(step & 1));
}

bool haptics_busy(void)
{
	return pattern != NULL;
}
