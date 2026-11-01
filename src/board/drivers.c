/* The touch and sensor hub calls, passed to the watch's driver; a watch
 * without the part answers as if the chip were absent, so the main loop
 * skips it. */
#include "asteroid.h"
#include "input/touch.h"
#include "sensors/hub.h"

#define TOUCH(fn) (board->touch && board->touch->fn)
#define HUB(fn) (board->hub && board->hub->fn)

bool touch_init(void) { return TOUCH(init) && board->touch->init(); }
bool touch_poll(struct touch *t) { return TOUCH(poll) && board->touch->poll(t); }
void touch_inject(int x0, int y0, int x1, int y1, uint32_t ms)
{
	if (TOUCH(inject))
		board->touch->inject(x0, y0, x1, y1, ms);
}
void touch_sleep(bool on)
{
	if (TOUCH(sleep))
		board->touch->sleep(on);
}
uint32_t touch_next_ms(void) { return TOUCH(next_ms) ? board->touch->next_ms() : UINT32_MAX; }
void touch_stats(uint32_t *attentions, uint32_t *errors)
{
	if (TOUCH(stats))
		board->touch->stats(attentions, errors);
	else
		*attentions = *errors = 0;
}

bool hub_init(void) { return HUB(init) && board->hub->init(); }
void hub_poll(void)
{
	if (HUB(poll))
		board->hub->poll();
}
int hub_send(const uint8_t *p, unsigned n) { return HUB(send) ? board->hub->send(p, n) : -1; }
void hub_set_time(void)
{
	if (HUB(set_time))
		board->hub->set_time();
}
uint32_t hub_steps(void) { return HUB(steps) ? board->hub->steps() : HUB_NONE; }
void hub_heart_rate_on(bool on)
{
	if (HUB(heart_rate_on))
		board->hub->heart_rate_on(on);
}
uint32_t hub_heart_rate(uint8_t *status, unsigned *readings)
{
	return HUB(heart_rate) ? board->hub->heart_rate(status, readings) : HUB_NONE;
}
bool hub_tilted(void) { return HUB(tilted) && board->hub->tilted(); }
void hub_accel_on(bool on)
{
	if (HUB(accel_on))
		board->hub->accel_on(on);
}
bool hub_accel(int16_t v[3]) { return HUB(accel) && board->hub->accel(v); }
bool hub_can_sleep(void) { return !HUB(can_sleep) || board->hub->can_sleep(); }
void hub_sleep(bool asleep)
{
	if (HUB(sleep))
		board->hub->sleep(asleep);
}
void hub_status(void (*say)(const char *fmt, ...))
{
	if (HUB(status))
		board->hub->status(say);
	else
		say("hub: no driver on this watch\n");
}
