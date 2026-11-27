#include "handpose.h"
#include "ui/ui.h"

/* The 9:15 line stock AsteroidOS uses, both hands on 12 and both on 6:
 * stacked hands cover one radius, where a round screen is narrow. */
enum { LINE, UP, DOWN };
static const struct handpose poses[] = {
	[LINE] = { HANDS_POSE, 45, 135 },
	[UP] = { HANDS_POSE, 0, 0 },
	[DOWN] = { HANDS_POSE, 90, 90 },
};

/* By where each screen has a gap or the least to read: on 12 where a
 * button or the newest content is at the bottom or two columns meet in
 * the upper half, on 6 where the title is on top. */
static const uint8_t panel_pose[UI_PANELS] = {
	[UI_PANEL_LEFT] = DOWN,
	[UI_PANEL_RIGHT] = UP,
	[UI_PANEL_BOTTOM] = UP,
	[UI_PANEL_TOP] = UP,
};
static const uint8_t app_pose[] = {
	[UI_APP_MUSIC] = DOWN,
	[UI_APP_SETTINGS] = DOWN,
	[UI_APP_ALARM] = UP,
	[UI_APP_TIMER] = UP,
	[UI_APP_STOPWATCH] = UP,
	[UI_APP_WEATHER] = DOWN,
	[UI_APP_HEART_RATE] = UP,
	[UI_APP_FLASHLIGHT] = DOWN,
	[UI_APP_CALCULATOR] = DOWN,
	[UI_APP_DIAMONDS] = UP,
	[UI_APP_AGENDA] = UP,
	[UI_APP_ACTIVITY] = UP,
	[UI_APP_STEPS] = UP,
	[UI_APP_COMPASS] = DOWN,
};

struct handpose handpose_want(const struct handpose_view *v, struct handpose cur)
{
	if (!v->awake)
		return (struct handpose){ .owner = HANDS_TIME };
	if (v->calibrate)
		return (struct handpose){ .owner = HANDS_CALIBRATE };
	/* A pop-up is gone in seconds: hands already aside stay put. */
	if (v->modal)
		return cur.owner == HANDS_POSE ? cur : poses[UP];
	if (v->panel == UI_PANEL_FACE)
		return (struct handpose){ .owner = HANDS_TIME };
	if (v->panel == UI_PANEL_APP)
		return poses[v->app >= 0 && (unsigned)v->app < sizeof app_pose ? app_pose[v->app] : LINE];
	return poses[v->panel < UI_PANELS ? panel_pose[v->panel] : LINE];
}

static bool same(struct handpose a, struct handpose b)
{
	return a.owner == b.owner && a.minute == b.minute && a.hour == b.hour;
}

bool handpose_step(struct handpose_state *s, struct handpose want, uint32_t now)
{
	if (same(want, s->cur)) {
		s->waiting = false;
		return false;
	}
	if (!s->waiting) {
		s->waiting = true;
		s->since = now;
	}
	if (want.owner == HANDS_POSE && s->cur.owner != HANDS_CALIBRATE && now - s->since < HANDPOSE_SETTLE_MS)
		return false;
	s->cur = want;
	s->waiting = false;
	return true;
}

uint32_t handpose_next_ms(const struct handpose_state *s, uint32_t now)
{
	if (!s->waiting)
		return UINT32_MAX;
	uint32_t gone = now - s->since;
	return gone < HANDPOSE_SETTLE_MS ? HANDPOSE_SETTLE_MS - gone : 0;
}
