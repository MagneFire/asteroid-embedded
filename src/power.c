/* Nightstand mode is in effect while the watch is on its charger and
 * for the nightstand delay after it comes off, as asteroid-launcher's
 * nightstandDelayTimer keeps it, so a loose cable does not end it.
 *
 * The always-on face: shown while asleep unless turned off, drawn on
 * the CPU so the GPU can stay off, with the panel in idle mode at 18 %
 * brightness, AsteroidOS's low power mode level on this watch, or at
 * the board's own backlight code, which follows the light sensor. Each minute it moves by a random amount against burn-in, as the launcher's
 * burnInProtectionManager moves the grid: up to the watchface's margin,
 * times 1, 1/2 or 1/4 for the high, medium and low levels, and the left
 * indicator by up to two and four fin widths. */
#include <string.h>
#include "asteroid.h"
#include "frame.h"
#include "display/display.h"
#include "ui/ui.h"
#include "ui/stack.h"
#include "ui/rotation.h"
#include "gpu/paint.h"
#include "bt/bt.h"
#include "sensors/hub.h"
#include "sensors/tsl2584.h"
#include "sensors/auto_brightness.h"
#include "screenshot/screenshot.h"
#include "storage/storage.h"
#include "clockapps.h"
#include "power.h"
#include "sched.h"

#define AOD_BRIGHTNESS 18
/* How long the hub's own display takes to show after ambient mode is
 * set, as seen on catfish. */
#define HUB_SCREEN_MS 800
/* How long a peek waits for the room's light before it lights the
 * screen without it; catfish's hub sends its first reading after about
 * 330 ms. */
#define LUX_WAIT_MS 600
#define FIN_PX 6
/* Awake, the light sensor is read this often, a reading is ready after
 * the integration time, and a change of brightness takes the fade time,
 * as mce fades it. */
#define LIGHT_READ_MS 250
#define LIGHT_INTEGRATE_MS 60
#define LIGHT_FADE_MS 1000
#define DCS_FADE_STEP_MS 100
/* Within the day and a half before the 32 kHz counter of minnow wraps. */
#define MAX_SLEEP_MS (24 * 3600 * 1000u)

static bool have_display, have_ui, have_hub;
static struct power_policy policy = { .awake = true };
static struct nightstand stand;
static enum screen_state screen;
static uint32_t ambient_minute, asleep_t, asleep_ms, wakeups, hub_ambient_t;
#if UI_LAYER_SIZE
static uint32_t *const layers = (uint32_t *)(uintptr_t)UI_LAYER_BASE;
#endif
static const char *wake_cause;
static bool lit_pending, lent, dark;
static enum { LUX_IDLE, LUX_READING, LUX_WATCHING, LUX_DONE } lux_state;
static uint32_t lux_t;
static int32_t lux_seen = -1;
static struct backlight lit, lit_fixed;
static bool lit_is_fixed, lit_shown;

void power_init(bool display, bool ui, bool hub)
{
	have_display = display;
	have_ui = ui;
	have_hub = hub;
	policy.last_activity = time_ms();
}

void power_request(bool sleep)
{
	if (sleep)
		policy.sleep_request = true;
	else
		policy.wake_request = true;
}

void power_wake(const char *cause)
{
	if (!policy.wake_request)
		wake_cause = cause;
	policy.wake_request = true;
}

void power_wake_cancel(const char *cause)
{
	if (policy.wake_request && wake_cause == cause) {
		policy.wake_request = false;
		wake_cause = NULL;
	}
}

/* The white screen lights the hub's own display from behind, so it is
 * the always-on face where there is none. */
void power_peek(const char *cause, bool light)
{
	if (policy.awake)
		return;
	if (!policy.peeking)
		log_printf("peek: %s\n", cause);
	power_policy_peek(&policy, time_ms(), light && have_hub && hub_has_screen());
}

void power_activity(uint32_t now)
{
	policy.last_activity = now;
	frame_busy(now);
}

void power_lend_display(bool on)
{
	lent = on;
}

void power_dark_until_wake(void)
{
	dark = true;
}

void power_keep_awake(bool on)
{
	policy.keep_awake = on;
}

/* As mce does. */
void power_charger_plugged(uint32_t now)
{
	power_policy_charger(&policy, now);
}

bool power_awake(void)
{
	return policy.awake;
}

static uint32_t nightstand_delay_ms(void)
{
	return (uint32_t)setting_get(SETTING_NIGHTSTAND_DELAY) * 1000;
}

bool power_nightstand(void)
{
	bool attached = nightstand_step(&stand, batt_cable(), time_ms(), nightstand_delay_ms());
	return setting_get(SETTING_NIGHTSTAND) && attached;
}

static enum screen_state screen_wanted(uint32_t now)
{
	if (!have_ui || lent || dark)
		return SCREEN_OFF;
	bool aod = power_policy_aod(power_nightstand(), setting_get(SETTING_NIGHTSTAND_AOD), setting_get(SETTING_AOD));
	return power_policy_screen(aod, power_policy_peeking(&policy, now), policy.peek_light);
}

static struct auto_brightness autob;
static bool light_on, light_reading;
static uint32_t light_t, fade_t;
static int fade_from, fade_to, fade_now;

static bool auto_brightness(void)
{
	return board->light && setting_get(SETTING_AUTO_BRIGHTNESS);
}

/* In nightstand mode the screen takes the nightstand brightness. With
 * automatic brightness that is the setting's curve at the light seen. */
static int brightness_wanted(void)
{
	int b = setting_get(power_nightstand() ? SETTING_NIGHTSTAND_BRIGHTNESS : SETTING_BRIGHTNESS);
	return auto_brightness() ? auto_brightness_percent(&autob, b) : b;
}

void power_set_brightness(void)
{
	fade_from = fade_to = fade_now = brightness_wanted();
	display_set_brightness((uint8_t)(fade_now * 255 / 100));
}

static void light_follow(bool on, uint32_t now)
{
	light_on = on;
	light_reading = false;
	light_keep(on);
	auto_brightness_reset(&autob);
	autob.levels = board->panel.brightness_levels;
	/* The first reading right away. */
	light_t = now - LIGHT_READ_MS;
}

void power_light(uint32_t now, bool hold)
{
	bool on = policy.awake && have_display && !hold && auto_brightness();
	if (on != light_on)
		light_follow(on, now);
	if (!on)
		return;
	if (light_reading && now - light_t >= LIGHT_INTEGRATE_MS) {
		light_reading = false;
		int32_t lux = light_finish();
		if (lux >= 0) {
			auto_brightness_add(&autob, lux);
			int b = brightness_wanted();
			if (b != fade_to) {
				log_printf("light: %ld lux, brightness %d\n", lux, b);
				fade_from = fade_now;
				fade_to = b;
				fade_t = now;
			}
		}
	} else if (!light_reading && now - light_t >= LIGHT_READ_MS) {
		light_start();
		light_reading = true;
		light_t = now;
	}
	/* Without a backlight chip each step is a panel command that waits
	 * for the frame being sent, so those panels take fewer steps. */
	static uint32_t step_t;
	if (fade_now != fade_to && (board->backlight || now - step_t >= DCS_FADE_STEP_MS)) {
		uint32_t gone = now - fade_t;
		int b = gone >= LIGHT_FADE_MS ? fade_to : fade_from + (fade_to - fade_from) * (int)gone / LIGHT_FADE_MS;
		if (b != fade_now) {
			fade_now = b;
			step_t = now;
			display_set_brightness((uint8_t)(b * 255 / 100));
		}
	}
}

/* Once a second while awake: the brightness follows nightstand mode. */
void power_tick(void)
{
	static bool ns;
	if (power_nightstand() != ns) {
		ns = !ns;
		power_set_brightness();
	}
}

#if UI_LAYER_SIZE
static int burn_in_shift(uint32_t *seed, int range)
{
	*seed ^= *seed << 13;
	*seed ^= *seed >> 17;
	*seed ^= *seed << 5;
	return range ? (int)(*seed % (uint32_t)(2 * range + 1)) - range : 0;
}
#endif

/* Back to the normal panel state and brightness, for waking or for the
 * panel turning off. */
static void ambient_leave(void)
{
	ui_set_ambient(false);
	display_idle(false);
	power_set_brightness();
}

/* The code for the light the sensor saw, the board's own without a
 * reading. */
static void ambient_backlight(void)
{
	static uint8_t shown;
	int32_t lux = light_finish();
	uint8_t code = lux < 0 ? board->backlight_aod_code : tsl2584_backlight_code((uint32_t)lux);
	if (code != shown)
		log_printf("light: %ld lux, code %u\n", lux, code);
	shown = code;
	display_set_backlight_code(code);
}

static void ambient_draw(void)
{
	if (board->backlight_aod_code)
		light_start();
	struct civil c;
	uint32_t rtc = rtc_read();
	civil_from_epoch(rtc, &c);
	ambient_minute = rtc / 60;
	ui_set_clock(c.hour, c.minute, c.second, c.weekday, c.day, c.month, c.year, rtc);
	ui_set_battery(batt_percent(), batt_charging());
	bool ns = power_nightstand();
	ui_set_face(setting_get(SETTING_WATCHFACE), setting_get(SETTING_NIGHTSTAND), ns);
	ui_tick();
#if UI_LAYER_SIZE
	uint32_t x, y, w, h, *buf = display_backbuffer();
	static const unsigned panels[] = { UI_PANEL_FACE, UI_PANEL_OVERLAY };
	for (unsigned i = 0; i < ARRAY_SIZE(panels); i++) {
		unsigned p = panels[i];
		if (ui_update(p, &x, &y, &w, &h)) {
			uint32_t *layer = layers + p * (UI_LAYER_STRIDE / 4);
			/* The GPU may have painted it: wait for that before the
			 * layer is scanned and copied, as frame_draw does. */
			paint_finish();
#ifdef SGX_BASE
			paint_wait();
#endif
			stack_damage(p, x, y, w, h);
			for (uint32_t r = y; r < y + h; r++)
				dcache_clean_range(layer + r * fb.stride + x, w * 4);
		}
	}
	/* The digital face leaves 54 px (20 px in nightstand mode) around
	 * it, the classic one 14 px (24 px). */
	static const uint8_t level[] = { 0, 4, 2, 1 };
	uint32_t seed = rtc | 1;
	int b = board->panel.lcd ? 0 : setting_get(SETTING_BURN_IN), m = level[b < 4 ? b : 0];
	int range = (setting_get(SETTING_WATCHFACE) ? (ns ? 24 : 14) : (ns ? 20 : 54)) * m / 4;
	int dx = burn_in_shift(&seed, range), dy = burn_in_shift(&seed, range);
	int ix = m ? burn_in_shift(&seed, FIN_PX) + FIN_PX : 0, iy = m ? burn_in_shift(&seed, 2 * FIN_PX) : 0;
	memset(buf, 0, fb.stride * fb.height * 4);
	ui_render(buf, fb.stride, dx, dy);
	stack_indicators_cpu(buf, fb.stride, ix, iy);
	rotation_buffer(buf, fb.stride);
	display_flip();
#else
	frame_ambient();
#endif
	if (board->backlight_aod_code)
		ambient_backlight();
}

/* The hub's own screen shows the time unless the main panel shows
 * something: awake, or the always-on face. */
static void hub_follow(bool want, uint32_t now)
{
	static bool normal = true;
	if (want != normal) {
		normal = want;
		hub_screen(want);
		if (!want)
			hub_ambient_t = now;
	}
}

/* From the always-on face the white screen waits for the hub's display,
 * which was off, so the two show together. */
static bool lit_waits(enum screen_state want, uint32_t now)
{
	return want == SCREEN_LIT && screen == SCREEN_AMBIENT && (int32_t)(now - hub_ambient_t) < HUB_SCREEN_MS;
}

/* The panel is on, the face goes up at its own brightness. */
static void ambient_enter(void)
{
	ui_set_ambient(true);
	ambient_draw();
	if (!board->backlight_aod_code)
		display_set_brightness(AOD_BRIGHTNESS * 255 / 100);
	display_idle(true);
}

static void light_fill(void)
{
	uint32_t px = lit.red ? (uint32_t)lit.pixel << 16 : lit.pixel * 0x010101u, *buf = display_backbuffer();
	for (uint32_t i = 0, n = fb.stride * fb.height; i < n; i++)
		buf[i] = px;
	display_flip();
}

static void backlight_pick(int32_t lux, uint32_t now)
{
	lux_seen = lux;
	lit = lit_is_fixed ? lit_fixed : power_policy_backlight(lux, setting_get(SETTING_BACKLIGHT_COLOR));
	log_printf("backlight: %ld lux after %lu ms, %u/%u %s\n", (long)lux, (unsigned long)(now - lux_t), lit.dbv, lit.pixel,
		   lit.red ? "red" : "white");
}

/* The room's light is read once per peek, before the screen lights, as
 * the sensor would see the screen. */
static bool backlight_ready(uint32_t now)
{
	if (lux_state == LUX_DONE)
		return true;
	if (lux_state == LUX_IDLE) {
		lux_state = LUX_READING;
		lux_t = now;
		light_keep(true);
		light_start();
		return false;
	}
	int32_t lux = light_finish();
	if (lux_state == LUX_WATCHING) {
		if (lux <= BACKLIGHT_DARK_LUX)
			return true;
		backlight_pick(lux, now);
		light_keep(false);
		lux_state = LUX_DONE;
		/* Too bright for the backlight, the caller turns it off. */
		if (screen == SCREEN_LIT && lit.dbv) {
			cpu_clock_slow(false);
			light_fill();
			display_set_brightness(lit.dbv);
			cpu_clock_slow(true);
		}
		return true;
	}
	if (lux < 0 && now - lux_t < LUX_WAIT_MS)
		return false;
	backlight_pick(lux, now);
	/* The hub reads no lower than the dark level, which is also what a
	 * sleeve over the sensor gives: the sensor stays on through the peek
	 * for one that comes off. */
	if (lux >= 0 && lux <= BACKLIGHT_DARK_LUX && !lit_is_fixed) {
		lux_state = LUX_WATCHING;
		return true;
	}
	light_keep(false);
	lux_state = LUX_DONE;
	return true;
}

/* The panel keeps the fill and its brightness through sleep in, which
 * the next wake would show at the normal brightness. */
static void lit_clear(void)
{
	memset(display_backbuffer(), 0, fb.stride * fb.height * 4);
	display_flip();
	power_set_brightness();
	lit_shown = false;
}

/* The reading comes with the hub's interrupt, so the wait for it can
 * sleep, up to its end. */
static uint32_t lux_next_ms(uint32_t now)
{
	if (lux_state != LUX_READING)
		return UINT32_MAX;
	uint32_t gone = now - lux_t;
	return gone >= LUX_WAIT_MS ? 1 : LUX_WAIT_MS - gone;
}

static uint32_t hub_screen_next_ms(uint32_t now)
{
	if (!lit_pending)
		return UINT32_MAX;
	uint32_t gone = now - hub_ambient_t;
	return gone >= HUB_SCREEN_MS ? 1 : HUB_SCREEN_MS - gone;
}

static void backlight_done(void)
{
	if (lux_state == LUX_READING || lux_state == LUX_WATCHING)
		light_keep(false);
	lux_state = LUX_IDLE;
}

/* Asleep, from what the panel shows to what it should show. */
static void screen_set(enum screen_state to)
{
	static const char *const names[] = { [SCREEN_AMBIENT] = "always-on", [SCREEN_LIT] = "backlight" };
	enum screen_state from = screen;
	if (to == from)
		return;
	cpu_clock_slow(false);
	if (to == SCREEN_OFF) {
		if (from == SCREEN_AMBIENT)
			ambient_leave();
		display_sleep();
	} else {
		/* The level goes in before sleep out, as the vendor sets it,
		 * so a panel that ramps to it does so out of sight. */
		if (to == SCREEN_LIT && from == SCREEN_OFF)
			display_set_brightness(lit.dbv);
		if (from == SCREEN_OFF)
			display_wake_hidden();
		if (to == SCREEN_AMBIENT) {
			lit_shown = false;
			ambient_enter();
		} else {
			light_fill();
			if (from == SCREEN_AMBIENT) {
				ui_set_ambient(false);
				display_idle(false);
				display_set_brightness(lit.dbv);
			}
			lit_shown = true;
		}
		if (from == SCREEN_OFF)
			display_show();
	}
	screen = to;
	if (from != SCREEN_OFF)
		log_printf("power: %s off\n", names[from]);
	if (to != SCREEN_OFF)
		log_printf("power: %s on\n", names[to]);
	cpu_clock_slow(true);
}

/* Asleep, the end of a peek is a deadline, and off the charger the end
 * of the nightstand delay is one if the normal setting differs from
 * what is on screen. */
uint32_t power_next_ms(void)
{
	if (policy.awake)
		return UINT32_MAX;
	uint32_t now = time_ms(), next = power_policy_peek_next_ms(&policy, now), lux = lux_next_ms(now);
	if (lux < next)
		next = lux;
	if ((screen == SCREEN_AMBIENT) != (have_ui && setting_get(SETTING_AOD)) && !batt_cable()) {
		uint32_t stand_next = nightstand_next_ms(&stand, now, nightstand_delay_ms());
		if (stand_next < next)
			next = stand_next;
	}
	return next;
}

enum power_change power_update(uint32_t *now_p)
{
	uint32_t now = *now_p;
	enum power_change change = POWER_NONE;
	if (!policy.awake) {
		wakeups++;
		/* Keeps the time the cable left, whatever else is asleep. */
		power_nightstand();
	}
	if (lent) {
		policy.wake_request = policy.charger_wake = policy.peeking = false;
		policy.sleep_request = policy.awake;
	}
	enum power_step step = power_policy_step(&policy, have_display, now);
	if (step == POWER_STEP_SLEEP) {
		/* Before the always-on face reads the sensor its own way. */
		light_follow(false, now);
		clockapps_sleep();
		if (store_quiet())
			settings_flush();
		screen = screen_wanted(now);
		if (screen == SCREEN_OFF)
			display_sleep();
		if (have_ui) {
			stack_sleep(now);
			ui_hide_notification();
		}
		if (screen == SCREEN_AMBIENT)
			ambient_enter();
		if (have_hub)
			hub_asleep(true);
		batt_asleep(true);
		timer_set_period(1000);
		cpu_clock_slow(true);
		policy.awake = false;
		asleep_t = now;
		log_printf("sleep\n");
		change = POWER_SLEPT;
	} else if (step == POWER_STEP_WAKE) {
		dark = false;
		cpu_clock_slow(false);
		if (have_hub)
			hub_asleep(false);
		batt_asleep(false);
		/* The tick armed while asleep can be up to a second away, and
		 * the touch drivers retry from the passes it brings. */
		timer_set_period(1);
		timer_next(1);
		backlight_done();
		if (screen == SCREEN_AMBIENT) {
			ambient_leave();
		} else if (screen == SCREEN_OFF && lit_shown) {
			display_wake_hidden();
			lit_clear();
			display_show();
		} else if (screen == SCREEN_OFF) {
			display_wake();
		} else {
			lit_clear();
		}
		screen = SCREEN_OFF;
		policy.awake = true;
		asleep_ms += now - asleep_t;
		now = time_ms();
		const char *cause = policy.charger_wake ? "charger" : wake_cause ? wake_cause : "other";
		power_policy_woke(&policy, now);
		if (have_ui)
			stack_wake(now);
		log_printf("wake: %s\n", cause);
		change = POWER_WOKE;
	}
	policy.sleep_request = policy.wake_request = false;
	wake_cause = NULL;
	/* Asleep on the charger the nightstand setting picked; once off the
	 * charger the normal one applies, either way. The picture stays off
	 * until the face is drawn, or the last screen would flash up. */
	if (!policy.awake) {
		enum screen_state want = screen_wanted(now);
		hub_follow(want == SCREEN_AMBIENT, now);
		bool reading = false;
		if (want == SCREEN_LIT)
			reading = !backlight_ready(now);
		else
			backlight_done();
		lit_pending = !reading && lit_waits(want, now);
		/* Too bright for the backlight to help: the hub's display shows
		 * with the screen dark. */
		if (!reading && !lit_pending)
			screen_set(want == SCREEN_LIT && !lit.dbv ? SCREEN_OFF : want);
	} else {
		lit_pending = false;
		hub_follow(true, now);
	}
	if (screen == SCREEN_AMBIENT && !policy.awake && rtc_read() / 60 != ambient_minute) {
		cpu_clock_slow(false);
		ambient_draw();
		cpu_clock_slow(true);
	}
	*now_p = now;
	return change;
}

/* Asleep (or without a display), wait for the next thing to do: the
 * next tick while anything is pending, else as long as app_next_ms and
 * the Bluetooth and sensor hub links allow, with the cluster parked
 * when they can sleep too. */
/* Asleep, what ended each wait: the deadline that was due, or the
 * interrupt that came before it. A pass that did not wait at all counts
 * as "busy". */
#define WOKE_KINDS 8
static struct woke {
	const char *name;
	unsigned irq;
	uint32_t n;
} woke[WOKE_KINDS];

static void woke_count(const char *name, unsigned irq)
{
	for (unsigned i = 0; i < WOKE_KINDS; i++)
		if (woke[i].n && woke[i].name == name && woke[i].irq == irq) {
			woke[i].n++;
			return;
		}
	for (unsigned i = 0; i < WOKE_KINDS - 1; i++)
		if (!woke[i].n) {
			woke[i] = (struct woke){ name, irq, 1 };
			return;
		}
	woke[WOKE_KINDS - 1].name = "other";
	woke[WOKE_KINDS - 1].irq = 0;
	woke[WOKE_KINDS - 1].n++;
}

const char *power_woke_by(void)
{
	static char s[160];
	unsigned len = 0;
	s[0] = 0;
	for (;;) {
		struct woke *top = NULL;
		for (unsigned i = 0; i < WOKE_KINDS; i++)
			if (woke[i].n && (!top || woke[i].n > top->n))
				top = &woke[i];
		if (!top || len >= sizeof s - 1)
			break;
		int w;
		if (!top->name) {
			unsigned base = top->irq >= IRQ_LAST_MPM ? IRQ_LAST_MPM : top->irq >= IRQ_LAST_GPIO ? IRQ_LAST_GPIO : 0;
			w = snprintf(s + len, sizeof s - len, "%s%lu %s %u", len ? ", " : "", top->n,
				     base == IRQ_LAST_MPM ? "mpm" : base ? "gpio" : "irq", top->irq - base);
		} else
			w = snprintf(s + len, sizeof s - len, "%s%lu %s", len ? ", " : "", top->n, top->name);
		if (w > 0)
			len += (unsigned)w;
		top->n = 0;
	}
	memset(woke, 0, sizeof woke);
	return len ? s : "nothing";
}

void power_idle(uint32_t app_next_ms, const char *why)
{
	/* A USB completion that landed after this pass's poll would wait
	 * for the next tick, a second asleep, and adb needs several per
	 * message: go round again instead, at full speed, as a pass on the
	 * sleep clock takes over 100 ms. */
	if (usb_busy()) {
		if (!policy.awake && cpu_clock_is_slow())
			cpu_clock_slow(false);
		if (!policy.awake)
			woke_count("busy", 0);
		return;
	}
	if ((have_display && policy.awake) || haptics_busy() || screenshot_busy() || !timer_ticks() || sched_pending()) {
		if (!policy.awake)
			woke_count("busy", 0);
		return;
	}
	bool quiet = !have_display || policy.awake || display_rest();
	bool cluster = quiet && !policy.awake && usb_resting() && bt_can_sleep() && (!have_hub || hub_can_sleep());
	if (!policy.awake)
		cpu_clock_slow(true);
	if (cluster) {
		bt_sleep(true);
		hub_sleep(true);
		cpu_clock_park();
	}
	uint32_t next = 1000;
	if (usb_connected())
		why = "usb tick";
	if (!policy.awake && !usb_connected()) {
		uint32_t bt = bt_idle_ms(), lpm = bt_lpm_next_ms();
		next = app_next_ms;
		if (bt < next) {
			next = bt;
			why = "bt";
		}
		if (lpm < next) {
			next = lpm;
			why = "bt sleep";
		}
		uint32_t minute = (60 - rtc_read() % 60) * 1000;
		if (screen == SCREEN_AMBIENT && minute < next) {
			next = minute;
			why = "ambient minute";
		}
		if (next > MAX_SLEEP_MS) {
			next = MAX_SLEEP_MS;
			why = "longest sleep";
		}
	}
	if (!policy.awake && have_hub && !hub_can_sleep() && next > 50) {
		next = 50;
		why = "hub busy";
	}
	uint32_t lux = lux_next_ms(time_ms()), hub = hub_screen_next_ms(time_ms());
	if (lux < next) {
		next = lux;
		why = "light";
	}
	if (hub < next) {
		next = hub;
		why = "hub screen";
	}
	/* The watchdog counts on the sleep clock through a cluster power
	 * collapse, where its bark cannot wake the core, so a wake that
	 * comes late would end in a bite with no record. As the vendor
	 * kernel does for suspend, it is off across a long or cluster sleep,
	 * and a wake later than planned is logged instead. */
	bool long_sleep = !policy.awake && (next > 1000 || cluster || lux != UINT32_MAX || hub != UINT32_MAX);
	if (long_sleep) {
		timer_set_period(next);
		timer_next(next);
		wdt_stop();
	}
	uint32_t t0 = time_ms();
	irq_last = ~0u;
	cpu_idle(cluster);
	uint32_t slept = time_ms() - t0;
	if (!policy.awake) {
		if (slept + 1 >= next)
			woke_count(why, 0);
		else
			woke_count(irq_last == ~0u ? "busy" : NULL, irq_last == ~0u ? 0 : irq_last);
	}
	if (long_sleep) {
		wdt_arm(3000, 4000, NULL);
		timer_set_period(1000);
		timer_next(1);
		if (slept > next + 1000)
			log_printf("power: woke %lu ms after a %lu ms sleep\n", slept, next);
	}
	if (cluster) {
		cpu_clock_unpark();
		hub_sleep(false);
		bt_sleep(false);
		cpu_clock_slow(false);
	}
}

int32_t power_backlight_seen(struct backlight *b)
{
	*b = lit;
	return lux_seen;
}

void power_backlight_fix(const struct backlight *b)
{
	lit_is_fixed = b;
	if (b)
		lit_fixed = *b;
}

uint32_t power_wakeups(void)
{
	uint32_t n = wakeups;
	wakeups = 0;
	return n;
}

/* Time asleep since the last call. */
uint32_t power_slept_ms(uint32_t now)
{
	uint32_t slept = asleep_ms + (policy.awake ? 0 : now - asleep_t);
	asleep_ms = 0;
	if (!policy.awake)
		asleep_t = now;
	return slept;
}

/* Timepiece: the hub shows the time on its own display with the watch
 * off. With a charger in, the PMIC powers straight back on, so there the
 * watch waits with its screen off instead: the power key restarts it,
 * and pulling the cable powers it off for real. */
void power_timepiece(void)
{
	hub_timepiece();
	if (!batt_cable()) {
		power_off_arm();
		reboot();
	}
	if (have_display)
		display_sleep();
	log_printf("timepiece: on the charger, waiting for the power key\n");
	for (;;) {
		wdt_feed();
		batt_poll();
		hub_poll();
		if (power_key()) {
			log_printf("timepiece: power key, restarting\n");
			reboot();
		}
		if (!batt_cable()) {
			log_printf("timepiece: charger gone, powering off\n");
			power_off_arm();
			reboot();
		}
		delay_ms(100);
	}
}
