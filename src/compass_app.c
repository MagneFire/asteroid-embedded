#include "ui/ui.h"
#include "sensors/hub.h"
#include "compass_app.h"

static struct compass_view view;
static bool sensing;

void compass_view_reset(struct compass_view *v) { *v = (struct compass_view){ 0, UI_COMPASS_NO_DATA, 0 }; }

bool compass_view_update(struct compass_view *v, float degrees, int accuracy, uint32_t now)
{
	int32_t state = accuracy <= 0 ? UI_COMPASS_NO_DATA : accuracy < 3 ? UI_COMPASS_CALIBRATE : UI_COMPASS_READY;
	bool shown = v->state != UI_COMPASS_NO_DATA, changed = state != v->state;
	v->state = state;
	if (state == UI_COMPASS_NO_DATA)
		return changed;
	if (degrees < 0 || degrees >= 360)
		degrees = 0;
	float off = degrees - (float)v->degrees;
	if (off >= 180)
		off -= 360;
	else if (off < -180)
		off += 360;
	if (shown && ((off < COMPASS_HOLD && off > -COMPASS_HOLD) || now - v->shown_t < COMPASS_SHOW_MS))
		return changed;
	int32_t d = (int32_t)(degrees + 0.5f) % 360;
	changed |= d != v->degrees;
	v->degrees = d;
	v->shown_t = now;
	return changed;
}

static void show(void) { ui_set_heading((uint32_t)view.degrees, (uint32_t)view.state); }

static void no_data(void)
{
	compass_view_reset(&view);
	show();
}

void compass_app_page(bool compass)
{
	if (compass)
		no_data();
	else
		compass_app_pass(false, 0);
}

void compass_app_sleep(void)
{
	compass_app_pass(false, 0);
	no_data();
}

void compass_app_pass(bool on, uint32_t now)
{
	if (on != sensing) {
		sensing = on;
		hub_compass_on(on);
		if (on)
			no_data();
	}
	if (!on)
		return;
	float heading = 0;
	int accuracy = hub_heading(&heading);
	if (compass_view_update(&view, heading, accuracy, now))
		show();
}
