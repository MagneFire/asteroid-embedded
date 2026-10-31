/* Vibration patterns: a pattern is a list of on/off durations, played
 * from the main loop on the board's motor driver (haptics.h). */
#include "asteroid.h"
#include "input/haptics.h"

static bool ready;
static const uint16_t *pattern;
static unsigned steps, step;
static uint32_t step_t;

bool haptics_init(void)
{
	ready = haptics_motor_init();
	return ready;
}

void haptics_play(const uint16_t *on_off_ms, unsigned n)
{
	if (!ready)
		return;
	if (pattern)
		haptics_motor(false);
	pattern = n ? on_off_ms : NULL;
	steps = n;
	step = 0;
	step_t = time_ms();
	if (pattern)
		haptics_motor(true);
}

void haptics_poll(void)
{
	if (!pattern || time_ms() - step_t < pattern[step])
		return;
	step_t = time_ms();
	if (++step == steps) {
		pattern = NULL;
		haptics_motor(false);
		return;
	}
	haptics_motor(!(step & 1));
}

bool haptics_busy(void)
{
	return pattern != NULL;
}
