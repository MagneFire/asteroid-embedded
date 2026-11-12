/* The battery, touch, sensor hub and light sensor calls, passed to the watch's driver;
 * a watch without the part answers as if the chip were absent, so the
 * main loop skips it. Without a battery driver nothing touches the
 * charger, and the percentage is the gauge's own "not known yet". */
#include "asteroid.h"
#include "input/touch.h"
#include "sensors/hub.h"

#define BATT(fn) (board->battery_driver && board->battery_driver->fn)
/* The calls that write to the charger, which a guessed watch might not
 * have wired the way its table says. */
#define BATT_SETUP(fn) (!board_assumed && BATT(fn))
#define TOUCH(fn) (board->touch && board->touch->fn)
#define HUB(fn) (board->hub && board->hub->fn)
#define LIGHT(fn) (board->light && board->light->fn)

void batt_init(void)
{
	if (BATT_SETUP(init))
		board->battery_driver->init();
}
void batt_poll(void)
{
	if (BATT_SETUP(poll))
		board->battery_driver->poll();
}
uint32_t batt_next_ms(void) { return BATT(next_ms) ? board->battery_driver->next_ms() : UINT32_MAX; }
uint32_t batt_percent(void) { return BATT(percent) ? board->battery_driver->percent() : 50; }
bool batt_charging(void) { return BATT(charging) && board->battery_driver->charging(); }
bool batt_cable(void) { return BATT(cable) && board->battery_driver->cable(); }
bool batt_usb(void) { return BATT(usb) ? board->battery_driver->usb() : batt_cable(); }
void batt_cable_irq_init(void)
{
	if (BATT_SETUP(cable_irq_init))
		board->battery_driver->cable_irq_init();
}
bool batt_cable_irq(void) { return BATT(cable_irq) && board->battery_driver->cable_irq(); }
uint32_t batt_mv(void) { return BATT(mv) ? board->battery_driver->mv() : 0; }
int32_t batt_ua(void) { return BATT(ua) ? board->battery_driver->ua() : 0; }
int32_t batt_avg_ua(void) { return BATT(avg_ua) ? board->battery_driver->avg_ua() : 0; }
const char *batt_status(void) { return BATT(status) ? board->battery_driver->status() : "no battery driver"; }

void light_init(void)
{
	if (!board_assumed && LIGHT(init))
		board->light->init();
}
void light_start(void)
{
	if (LIGHT(start))
		board->light->start();
}
void light_keep(bool on)
{
	if (LIGHT(keep))
		board->light->keep(on);
}
int32_t light_finish(void) { return LIGHT(finish) ? board->light->finish() : -1; }

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
bool hub_measures_activity(void) { return HUB(activity); }
bool hub_activity(uint32_t v[2]) { return HUB(activity) && board->hub->activity(v); }
bool hub_has_passive(void) { return HUB(passive); }
void hub_passive_on(bool on)
{
	if (HUB(passive_on))
		board->hub->passive_on(on);
}
unsigned hub_passive(uint32_t *t, uint8_t *bpm, unsigned max) { return HUB(passive) ? board->hub->passive(t, bpm, max) : 0; }
void hub_profile(const struct hub_profile *p)
{
	if (HUB(profile))
		board->hub->profile(p);
}
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
bool hub_tilt_on(bool on)
{
	return !HUB(tilt_on) || board->hub->tilt_on(on);
}

void hub_accel_on(bool on)
{
	if (HUB(accel_on))
		board->hub->accel_on(on);
}
bool hub_accel(int16_t v[3]) { return HUB(accel) && board->hub->accel(v); }
bool hub_can_sleep(void) { return !HUB(can_sleep) || board->hub->can_sleep(); }
int hub_raw(uint8_t bank, uint8_t offset, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	return HUB(raw) ? board->hub->raw(bank, offset, w, wn, r, rn) : -1;
}
void hub_sleep(bool asleep)
{
	if (HUB(sleep))
		board->hub->sleep(asleep);
}
void hub_screen(bool on)
{
	if (HUB(screen))
		board->hub->screen(on);
}
void hub_status(void (*say)(const char *fmt, ...))
{
	if (HUB(status))
		board->hub->status(say);
	else
		say("hub: no driver on this watch\n");
}
