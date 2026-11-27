/* Who has the hands, and where they go for what is on show. */
#include <stdio.h>
#include "handpose.h"
#include "ui/ui.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static struct handpose_state s;
static struct handpose_view v;

static bool step(uint32_t now) { return handpose_step(&s, handpose_want(&v, s.cur), now); }
static bool at(unsigned owner, unsigned minute, unsigned hour)
{
	return s.cur.owner == owner && s.cur.minute == minute && s.cur.hour == hour;
}

/* Each screen's pose, and the 9:15 line for an app the table lacks. */
static void poses(void)
{
	const struct handpose none = { HANDS_TIME };
	struct handpose p;
	v = (struct handpose_view){ .awake = true, .panel = UI_PANEL_FACE };
	CHECK(handpose_want(&v, none).owner == HANDS_TIME);
	v.panel = UI_PANEL_LEFT;
	p = handpose_want(&v, none);
	CHECK(p.owner == HANDS_POSE && p.minute == 90 && p.hour == 90);
	v.panel = UI_PANEL_TOP;
	p = handpose_want(&v, none);
	CHECK(p.owner == HANDS_POSE && p.minute == 0 && p.hour == 0);
	v.panel = UI_PANEL_APP;
	v.app = UI_APP_TIMER;
	p = handpose_want(&v, none);
	CHECK(p.minute == 0 && p.hour == 0);
	v.app = UI_APP_SETTINGS;
	p = handpose_want(&v, none);
	CHECK(p.minute == 90 && p.hour == 90);
	v.app = 99;
	p = handpose_want(&v, none);
	CHECK(p.owner == HANDS_POSE && p.minute == 45 && p.hour == 135);
	v.app = -1;
	p = handpose_want(&v, none);
	CHECK(p.minute == 45 && p.hour == 135);
	v.awake = false;
	CHECK(handpose_want(&v, none).owner == HANDS_TIME);
}

/* A swipe past a screen leaves the hands on the time; staying moves them
 * once, to the pose wanted when the wait is over. */
static void settles(void)
{
	s = (struct handpose_state){ 0 };
	v = (struct handpose_view){ .awake = true, .panel = UI_PANEL_BOTTOM };
	CHECK(!step(1000) && handpose_next_ms(&s, 1000) == HANDPOSE_SETTLE_MS);
	v.panel = UI_PANEL_FACE;
	CHECK(!step(1300) && handpose_next_ms(&s, 1300) == UINT32_MAX);
	v.panel = UI_PANEL_BOTTOM;
	CHECK(!step(2000));
	v.panel = UI_PANEL_APP;
	v.app = UI_APP_SETTINGS;
	CHECK(!step(2300) && handpose_next_ms(&s, 2300) == 200);
	CHECK(step(2500) && at(HANDS_POSE, 90, 90));
	CHECK(!step(2600) && handpose_next_ms(&s, 2600) == UINT32_MAX);
}

/* From one pose to another the hands go straight there after the same
 * wait, and back to the time at once. */
static void pose_to_pose(void)
{
	s = (struct handpose_state){ .cur = { HANDS_POSE, 90, 90 } };
	v = (struct handpose_view){ .awake = true, .panel = UI_PANEL_APP, .app = UI_APP_TIMER };
	CHECK(!step(1000));
	CHECK(at(HANDS_POSE, 90, 90));
	CHECK(step(1500) && at(HANDS_POSE, 0, 0));
	v.panel = UI_PANEL_FACE;
	CHECK(step(1600) && at(HANDS_TIME, 0, 0));
	v.panel = UI_PANEL_APP;
	CHECK(!step(1700));
	v.awake = false;
	CHECK(!step(1800) && at(HANDS_TIME, 0, 0) && handpose_next_ms(&s, 1800) == UINT32_MAX);
}

/* A pop-up moves hands on the time aside, and leaves a pose alone. */
static void popup(void)
{
	s = (struct handpose_state){ 0 };
	v = (struct handpose_view){ .awake = true, .modal = true, .panel = UI_PANEL_FACE };
	CHECK(!step(0) && step(500) && at(HANDS_POSE, 0, 0));
	v.modal = false;
	CHECK(step(600) && at(HANDS_TIME, 0, 0));
	s.cur = (struct handpose){ HANDS_POSE, 90, 90 };
	v = (struct handpose_view){ .awake = true, .modal = true, .panel = UI_PANEL_APP, .app = UI_APP_SETTINGS };
	CHECK(!step(1000) && !step(2000) && at(HANDS_POSE, 90, 90));
}

/* Calibration takes the hands at once, to 12, and gives them back at
 * once to the pose or the time; asleep it does not hold them. */
static void calibration(void)
{
	s = (struct handpose_state){ .cur = { HANDS_POSE, 90, 90 } };
	v = (struct handpose_view){ .awake = true, .calibrate = true, .panel = UI_PANEL_APP, .app = UI_APP_SETTINGS };
	CHECK(step(1000) && at(HANDS_CALIBRATE, 0, 0));
	CHECK(!step(1100));
	v.calibrate = false;
	CHECK(step(1200) && at(HANDS_POSE, 90, 90));
	v.calibrate = true;
	CHECK(step(1300) && at(HANDS_CALIBRATE, 0, 0));
	v.awake = false;
	CHECK(step(1400) && at(HANDS_TIME, 0, 0));
	v.awake = true;
	CHECK(step(1500) && at(HANDS_CALIBRATE, 0, 0));
	v.calibrate = false;
	v.panel = UI_PANEL_FACE;
	CHECK(step(1600) && at(HANDS_TIME, 0, 0));
}

int main(void)
{
	poses();
	settles();
	pose_to_pose();
	popup();
	calibration();
	if (failures)
		return 1;
	printf("handpose_test: ok\n");
	return 0;
}
