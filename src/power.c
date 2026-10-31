/* Nightstand mode is in effect while the watch is on its charger and
 * for the nightstand delay after it comes off, as asteroid-launcher's
 * nightstandDelayTimer keeps it, so a loose cable does not end it.
 *
 * The always-on face: shown while asleep unless turned off, drawn on
 * the CPU so the GPU can stay off, with the panel in idle mode at 18 %
 * brightness, AsteroidOS's low power mode level on this watch. Each
 * minute it moves by a random amount against burn-in, as the launcher's
 * burnInProtectionManager moves the grid: up to the watchface's margin,
 * times 1, 1/2 or 1/4 for the high, medium and low levels, and the left
 * indicator by up to two and four fin widths. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "ui/ui.h"
#include "ui/stack.h"
#include "bt/bt.h"
#include "sensors/hub.h"
#include "screenshot/screenshot.h"
#include "clockapps.h"
#include "power.h"

#define AOD_BRIGHTNESS 18
#define FIN_PX 6

static bool have_display, have_ui, have_hub;
static bool awake = true, ambient, sleep_request, wake_request, charger_wake;
static uint32_t ambient_minute, last_activity, asleep_t, asleep_ms, wakeups;
static bool keep_awake;
static uint32_t *const layers = (uint32_t *)(uintptr_t)UI_LAYER_BASE;

void power_init(bool display, bool ui, bool hub)
{
	have_display = display;
	have_ui = ui;
	have_hub = hub;
	last_activity = time_ms();
}

void power_request(bool sleep)
{
	if (sleep)
		sleep_request = true;
	else
		wake_request = true;
}

void power_activity(uint32_t now)
{
	last_activity = now;
}

void power_keep_awake(bool on)
{
	keep_awake = on;
}

/* A cable plugged in shows the screen for CHARGER_WAKE_MS, as mce
 * does. */
void power_charger_plugged(uint32_t now)
{
	if (!awake)
		charger_wake = true;
	else if (now - last_activity > SLEEP_AFTER_MS - CHARGER_WAKE_MS)
		last_activity = now - (SLEEP_AFTER_MS - CHARGER_WAKE_MS);
}

bool power_awake(void)
{
	return awake;
}

bool power_nightstand(void)
{
	static bool was_usb;
	static uint32_t lost_t;
	bool usb = pmic_usb_present();
	uint32_t now = time_ms();
	if (was_usb && !usb)
		lost_t = now | 1;
	was_usb = usb;
	return setting_get(SETTING_NIGHTSTAND) &&
	       (usb || (lost_t && now - lost_t < (uint32_t)setting_get(SETTING_NIGHTSTAND_DELAY) * 1000));
}

/* In nightstand mode the screen takes the nightstand brightness. */
void power_set_brightness(void)
{
	int b = setting_get(power_nightstand() ? SETTING_NIGHTSTAND_BRIGHTNESS : SETTING_BRIGHTNESS);
	display_set_brightness((uint8_t)(b * 255 / 100));
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

static int burn_in_shift(uint32_t *seed, int range)
{
	*seed ^= *seed << 13;
	*seed ^= *seed >> 17;
	*seed ^= *seed << 5;
	return range ? (int)(*seed % (uint32_t)(2 * range + 1)) - range : 0;
}

static void ambient_draw(void)
{
	struct civil c;
	uint32_t rtc = pmic_rtc_read(), x, y, w, h, *buf = display_backbuffer();
	civil_from_epoch(rtc, &c);
	ambient_minute = rtc / 60;
	ui_set_clock(c.hour, c.minute, c.second, c.weekday, c.day, c.month, c.year, rtc);
	ui_set_battery(bms_percent(), pmic_charging());
	bool ns = power_nightstand();
	ui_set_face(setting_get(SETTING_WATCHFACE), setting_get(SETTING_NIGHTSTAND), ns);
	ui_tick();
	static const unsigned panels[] = { UI_PANEL_FACE, UI_PANEL_OVERLAY };
	for (unsigned i = 0; i < ARRAY_SIZE(panels); i++) {
		unsigned p = panels[i];
		if (ui_update(p, &x, &y, &w, &h)) {
			uint32_t *layer = layers + p * (UI_LAYER_STRIDE / 4);
			stack_damage(p, x, y, w, h);
			for (uint32_t r = y; r < y + h; r++)
				dcache_clean_range(layer + r * fb.stride + x, w * 4);
		}
	}
	/* The digital face leaves 54 px (20 px in nightstand mode) around
	 * it, the classic one 14 px (24 px). */
	static const uint8_t level[] = { 0, 4, 2, 1 };
	uint32_t seed = rtc | 1;
	int b = setting_get(SETTING_BURN_IN), m = level[b < 4 ? b : 0];
	int range = (setting_get(SETTING_WATCHFACE) ? (ns ? 24 : 14) : (ns ? 20 : 54)) * m / 4;
	int dx = burn_in_shift(&seed, range), dy = burn_in_shift(&seed, range);
	int ix = m ? burn_in_shift(&seed, FIN_PX) + FIN_PX : 0, iy = m ? burn_in_shift(&seed, 2 * FIN_PX) : 0;
	memset(buf, 0, fb.stride * fb.height * 4);
	ui_render(buf, fb.stride, dx, dy);
	stack_indicators_cpu(buf, fb.stride, ix, iy);
	display_flip();
}

enum power_change power_update(uint32_t *now_p)
{
	uint32_t now = *now_p;
	enum power_change change = POWER_NONE;
	if (!awake)
		wakeups++;
	/* The short charger wake only when nothing else wakes the watch. */
	if (charger_wake && !awake && !wake_request)
		wake_request = true;
	else
		charger_wake = false;
	if (keep_awake && awake && !sleep_request)
		last_activity = now;
	if (have_display && awake && (sleep_request || now - last_activity >= SLEEP_AFTER_MS)) {
		clockapps_sleep();
		settings_flush();
		ambient = have_ui && setting_get(power_nightstand() ? SETTING_NIGHTSTAND_AOD : SETTING_AOD);
		if (!ambient)
			display_sleep();
		if (have_ui) {
			stack_sleep(now);
			ui_hide_notification();
		}
		if (ambient) {
			ui_set_ambient(true);
			ambient_draw();
			display_set_brightness(AOD_BRIGHTNESS * 255 / 100);
			display_idle(true);
		}
		timer_set_period(1000);
		cpu_clock_slow(true);
		awake = false;
		asleep_t = now;
		log_printf("sleep\n");
		change = POWER_SLEPT;
	} else if (have_display && !awake && wake_request) {
		cpu_clock_slow(false);
		timer_set_period(1);
		if (ambient) {
			ui_set_ambient(false);
			display_idle(false);
			power_set_brightness();
			ambient = false;
		} else {
			display_wake();
		}
		awake = true;
		asleep_ms += now - asleep_t;
		last_activity = now = time_ms();
		if (charger_wake)
			last_activity -= SLEEP_AFTER_MS - CHARGER_WAKE_MS;
		charger_wake = false;
		if (have_ui)
			stack_wake(now);
		log_printf("wake\n");
		change = POWER_WOKE;
	}
	sleep_request = wake_request = false;
	if (ambient && !awake && pmic_rtc_read() / 60 != ambient_minute) {
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
void power_idle(uint32_t app_next_ms)
{
	if ((have_display && awake) || haptics_busy() || screenshot_busy() || !timer_ticks())
		return;
	bool quiet = !have_display || awake || display_rest();
	bool cluster = quiet && !awake && !usb_connected() && bt_can_sleep() && (!have_hub || hub_can_sleep());
	if (!awake)
		cpu_clock_slow(true);
	if (cluster) {
		bt_sleep(true);
		hub_sleep(true);
		cpu_clock_park();
	}
	uint32_t next = 1000;
	if (!awake && !usb_connected()) {
		uint32_t bt = bt_idle_ms(), lpm = bt_lpm_next_ms();
		next = app_next_ms;
		if (bt < next)
			next = bt;
		if (lpm < next)
			next = lpm;
		if (have_hub && !hub_can_sleep() && next > 50)
			next = 50;
		uint32_t minute = (60 - pmic_rtc_read() % 60) * 1000;
		if (ambient && minute < next)
			next = minute;
	}
	bool long_sleep = !awake && next > 1000;
	if (long_sleep) {
		timer_set_period(next);
		timer_next(next);
		wdt_arm(next + 3000, next + 4000, NULL);
	}
	spm_idle(cluster);
	if (long_sleep) {
		wdt_arm(3000, 4000, NULL);
		timer_set_period(1000);
		timer_next(1);
	}
	if (cluster) {
		cpu_clock_unpark();
		hub_sleep(false);
		bt_sleep(false);
		cpu_clock_slow(false);
	}
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
	uint32_t slept = asleep_ms + (awake ? 0 : now - asleep_t);
	asleep_ms = 0;
	if (!awake)
		asleep_t = now;
	return slept;
}
