/* Vibration patterns: a pattern is a list of on/off durations, played
 * from the main loop on the watch's motor driver (board->motor). */
#include "asteroid.h"

static bool ready, held, started;
static const uint16_t *pattern;
static unsigned steps, step;
static uint32_t step_t;

/* Held from the start, the driver starts when it is let go. */
bool haptics_init(void)
{
	if (held)
		return board->motor != NULL;
	started = true;
	ready = board->motor && board->motor->init();
	return ready;
}

void haptics_hold(bool on)
{
	if (on && pattern) {
		pattern = NULL;
		board->motor->set(false);
	}
	if (on != held && started && board->motor && board->motor->lend) {
		bool up = board->motor->lend(on);
		if (!on)
			ready = up;
	}
	held = on;
	if (!on && !started && board->motor) {
		started = true;
		ready = board->motor->init();
	}
}

void haptics_play(const uint16_t *on_off_ms, unsigned n)
{
	if (!ready || held)
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

uint32_t haptics_next_ms(void)
{
	return pattern ? ms_left(step_t, pattern[step], time_ms()) : UINT32_MAX;
}
