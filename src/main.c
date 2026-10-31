/* Bring-up and the main loop. The display round: console on the firmware's
 * own buffers, then the FlatMesh background at the panel's pace with the
 * console as an overlay, then brightness and panel sleep/wake, then a
 * reboot so the bootloader is back in fastboot for the next image. A
 * build-time TEST hangs or faults on purpose to exercise the crash path. */
#include <stddef.h>
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "input/touch.h"
#include "console/console.h"
#include "gpu/a3xx.h"
#include "gpu/draw.h"
#include "gpu/paint.h"
#include "gpu/shaders.h"
#include "ui/ui.h"
#include "ui/stack.h"
#include "bt/bt.h"
#include "notify/notify.h"
#include "sensors/hub.h"
#include "media/media.h"
#include "weather/weather.h"
#include "screenshot/screenshot.h"
#include "storage/storage.h"
#include "alarm.h"
#include "config.h"

#define SLEEP_AFTER_MS 10000
#define MESH_STEP_MS 90
#define CHARGER_WAKE_MS 3000
#define RING_MS 30000
#define ASLEEP_STATS_MS 60000
#define BT_START_MS 2000

#ifndef REBOOT_AFTER_MS
#define REBOOT_AFTER_MS 0
#endif

extern void wdt_bark_handler(void);

static void sink_fbcon(const char *s, size_t n)
{
	char buf[257];
	while (n) {
		size_t k = n < 256 ? n : 256;
		memcpy(buf, s, k);
		buf[k] = 0;
		fbcon_puts(buf);
		s += k;
		n -= k;
	}
}

static int dot_x = -1, dot_y = -1;
static bool have_gpu;
static uint32_t gpu_frames, gpu_us, gpu_vert_us, gpu_fail;
static uint32_t *const layers = (uint32_t *)(uintptr_t)UI_LAYER_BASE;

static uint32_t mesh_t, app_center = 0x888888, app_outer;

/* An open app's own mesh in its colours, under it as it moves and fades. */
static void app_background(int x, int y, float alpha)
{
	static float *vbo;
	if (!vbo)
		vbo = gpu_alloc(FLATMESH_GPU_ROUND_VERTICES * 32, 64);
	unsigned n = flatmesh_vertices_gpu_look(vbo, mesh_t, app_center, app_outer, (float)x / fb.width,
						(float)y / fb.height, alpha);
	gpu_draw_program(&shader_c_vs, &shader_c_fs);
	gpu_draw_blend(alpha < 1.0f);
	gpu_draw_triangles((uint32_t)(uintptr_t)vbo, 32, 2, (const uint8_t[]){4, 4}, n);
	gpu_draw_blend(false);
}

static uint32_t rgb(struct ui_rgb c)
{
	return (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | c.b;
}

static void open_app(int app)
{
	struct ui_colors c;
	app_center = 0x888888;
	app_outer = 0;
	if (ui_app_colors(app, &c)) {
		app_center = rgb(c.center);
		app_outer = rgb(c.outer);
	}
	ui_set_app(app);
	stack_open_app();
}

static void gpu_flatmesh(uint32_t *buf, uint32_t t_ms, bool with_layer)
{
	static float *vbo;
	if (!vbo)
		vbo = gpu_alloc(FLATMESH_GPU_VERTICES * 32, 64);
	uint32_t t0 = time_us();
	mesh_t = t_ms;
	unsigned n = flatmesh_vertices_gpu(vbo, t_ms);
	struct gpu_target t = { (uint32_t)(uintptr_t)buf, fb.width, fb.height, fb.stride * 4, false };
	dcache_invalidate_range(buf, fb.stride * fb.height * 4);
	uint32_t t1 = time_us();
	gpu_draw_begin(&t);
	gpu_draw_program(&shader_c_vs, &shader_c_fs);
	gpu_draw_triangles((uint32_t)(uintptr_t)vbo, 32, 2, (const uint8_t[]){4, 4}, n);
	if (with_layer)
		stack_compose(app_background);
	if (!gpu_draw_end()) {
		if (gpu_fail++ == 0)
			log_printf("gpu: draw did not complete\n");
		have_gpu = false;
	}
	dcache_invalidate_range(buf, fb.stride * fb.height * 4);
	gpu_us += time_us() - t1;
	gpu_vert_us += t1 - t0;
	gpu_frames++;
}


static void draw_dot(uint32_t *buf)
{
	if (dot_x < 0)
		return;
	for (int dy = -6; dy <= 6; dy++)
		for (int dx = -6; dx <= 6; dx++) {
			int x = dot_x + dx, y = dot_y + dy;
			if (x >= 0 && y >= 0 && x < (int)fb.width && y < (int)fb.height && dx * dx + dy * dy <= 36)
				buf[y * fb.stride + x] = 0x00ffffff;
		}
}

static volatile bool console_sleep, console_wake, console_settings;

void settings_changed(void)
{
	console_settings = true;
}

void power_request(bool sleep)
{
	if (sleep)
		console_sleep = true;
	else
		console_wake = true;
}

/* Nightstand mode is in effect while the watch is on its charger and
 * for the nightstand delay after it comes off, as asteroid-launcher's
 * nightstandDelayTimer keeps it, so a loose cable does not end it. */
static bool nightstand_on(void)
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

/* The always-on face: shown while asleep unless turned off, drawn on
 * the CPU so the GPU can stay off, with the panel in idle mode at 18 %
 * brightness, AsteroidOS's low power mode level on this watch. Each
 * minute it moves by a random amount against burn-in, as the launcher's
 * burnInProtectionManager moves the grid: up to the watchface's margin,
 * times 1, 1/2 or 1/4 for the high, medium and low levels, and the left
 * indicator by up to two and four fin widths. */
#define AOD_BRIGHTNESS 18
#define FIN_PX 6
static bool ambient;
static uint32_t ambient_minute;

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
	bool ns = nightstand_on();
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

static void bluetooth_start(void)
{
	uint8_t addr[6];
	struct bt_info info;
	bool have_addr = !bt_addr_load(addr);
	if (bt_start(have_addr ? addr : NULL, &info)) {
		log_printf("bt: %s failed (%d)\n", info.failed, info.status);
		return;
	}
	int r = bt_host_start();
	if (r)
		log_printf("bt: host failed (%d)\n", r);
}

/* The mesh fades from its grey to the colours of the launcher page on
 * screen as the launcher comes in, t from 0 to 1. */
static void mesh_colors(float t)
{
	static uint32_t shown_center = 0x888888, shown_outer;
	uint32_t lc = 0x888888, lo = 0, c = 0, o = 0;
	struct ui_colors lcs;
	if (t > 0.0f && ui_launcher_colors(&lcs)) {
		lc = rgb(lcs.center);
		lo = rgb(lcs.outer);
	}
	for (int s = 0; s < 24; s += 8) {
		c |= (uint32_t)((float)(lc >> s & 0xff) * t + (float)(0x88) * (1 - t)) << s;
		o |= (uint32_t)((float)(lo >> s & 0xff) * t) << s;
	}
	if (c != shown_center || o != shown_outer) {
		shown_center = c;
		shown_outer = o;
		flatmesh_colors(c, o);
	}
}

static void show_settings(void)
{
	struct ui_settings u = {
		.brightness = setting_get(SETTING_BRIGHTNESS),
		.nightstand_brightness = setting_get(SETTING_NIGHTSTAND_BRIGHTNESS),
		.nightstand_delay = setting_get(SETTING_NIGHTSTAND_DELAY),
		.burn_in = setting_get(SETTING_BURN_IN),
		.bluetooth = setting_get(SETTING_BLUETOOTH),
		.vibration = setting_get(SETTING_VIBRATION),
		.aod = setting_get(SETTING_AOD),
		.clock12 = setting_get(SETTING_CLOCK12),
		.tilt = setting_get(SETTING_TILT),
		.tap = setting_get(SETTING_TAP),
		.fahrenheit = setting_get(SETTING_FAHRENHEIT),
		.nightstand_aod = setting_get(SETTING_NIGHTSTAND_AOD),
	};
	ui_set_settings(&u);
	ui_set_launcher(setting_get(SETTING_LAUNCHER));
	stack_set_launcher(setting_get(SETTING_LAUNCHER));
	ui_set_face(setting_get(SETTING_WATCHFACE), setting_get(SETTING_NIGHTSTAND), nightstand_on());
}

/* In nightstand mode the screen takes the nightstand brightness. */
static void set_brightness(void)
{
	int b = setting_get(nightstand_on() ? SETTING_NIGHTSTAND_BRIGHTNESS : SETTING_BRIGHTNESS);
	display_set_brightness((uint8_t)(b * 255 / 100));
}

/* A setting changed from the UI: store it and put it into effect. */
static void setting_toggled(int s)
{
	if (s == SETTING_BRIGHTNESS || s == UI_BRIGHTNESS_DOWN || s == UI_BRIGHTNESS_UP) {
		int b = setting_get(SETTING_BRIGHTNESS);
		if (s == SETTING_BRIGHTNESS)
			b = b >= 100 ? 25 : (b / 25 + 1) * 25;
		else
			b = s == UI_BRIGHTNESS_UP ? (b + 10 > 100 ? 100 : b + 10) : (b - 10 < 10 ? 10 : b - 10);
		setting_set(SETTING_BRIGHTNESS, b);
		set_brightness();
	} else if (s >= UI_WATCHFACE_FIRST && s < UI_WATCHFACE_FIRST + 2) {
		setting_set(SETTING_WATCHFACE, s - UI_WATCHFACE_FIRST);
	} else if (s >= UI_LAUNCHER_FIRST && s < UI_LAUNCHER_FIRST + UI_LAUNCHERS) {
		setting_set(SETTING_LAUNCHER, s - UI_LAUNCHER_FIRST);
	} else if (s == UI_LOCK) {
		power_request(true);
	} else if (s == UI_OPEN_SETTINGS) {
		open_app(UI_APP_SETTINGS);
	} else if (s == UI_CINEMA) {
		/* Cinema mode is the always-on face off, the state before it
		 * kept to go back to, as the quick panel's cinemaToggle does. */
		if (setting_get(SETTING_AOD)) {
			setting_set(SETTING_CINEMA_AOD, 1);
			setting_set(SETTING_AOD, 0);
		} else {
			setting_set(SETTING_AOD, setting_get(SETTING_CINEMA_AOD));
		}
	} else if (s == UI_REBOOT || s == UI_POWER_OFF) {
		log_printf("%s\n", s == UI_REBOOT ? "rebooting" : "powering off");
		if (s == UI_POWER_OFF)
			pmic_shutdown_mode();
		reboot();
	} else if (s == UI_PAIR_REJECT || s == UI_PAIR_ACCEPT) {
		bt_pairing_reply(s == UI_PAIR_ACCEPT);
	} else if (s == UI_FORGET_PHONE) {
		log_printf("bt: %s\n", bt_store_clear() ? "could not forget the phone" : "phone forgotten");
		bt_host_disconnect();
		ui_set_connected(bt_host_connected(), bt_store_bonded());
	} else if (s == SETTING_BLUETOOTH) {
		bool on = !setting_get(SETTING_BLUETOOTH);
		setting_set(SETTING_BLUETOOTH, on);
		if (on) {
			bluetooth_start();
		} else {
			bt_host_stop();
			bt_stop();
		}
	} else if (s == SETTING_NIGHTSTAND) {
		setting_set(SETTING_NIGHTSTAND, !setting_get(SETTING_NIGHTSTAND));
		set_brightness();
	} else if (s >= 0 && s < SETTINGS) {
		setting_set((enum setting)s, !setting_get((enum setting)s));
	}
	show_settings();
}

/* The timer and the stopwatch are kept in the key-value store across
 * reboots: the timer's duration and time left and, while it runs, the
 * RTC second it ends at; the stopwatch's time and laps and, while it
 * runs, the RTC second it started at. A timer that ended while the
 * watch was off is back at its duration. */
#define KV_TIMER	"timer"
#define KV_STOPWATCH	"stopwatch"
#define LAPS		99

struct saved_timer {
	uint32_t set_ms, left_ms, end_rtc;
};

struct saved_stopwatch {
	uint32_t acc_ms, start_rtc, running, laps;
	uint32_t lap_ms[LAPS];
};

static void save_timer(uint32_t set, uint32_t left, uint32_t end, uint32_t now)
{
	struct saved_timer v = { set, left, end ? pmic_rtc_read() + (end - now + 500) / 1000 : 0 };
	kv_set(KV_TIMER, &v, sizeof v);
}

static void save_stopwatch(uint32_t acc, bool running, uint32_t start, uint32_t now, const uint32_t *lap,
			   unsigned laps)
{
	struct saved_stopwatch v = {
		.acc_ms = acc,
		.start_rtc = running ? pmic_rtc_read() - (now - start) / 1000 : 0,
		.running = running,
		.laps = laps,
	};
	memcpy(v.lap_ms, lap, laps * sizeof lap[0]);
	kv_set(KV_STOPWATCH, &v, offsetof(struct saved_stopwatch, lap_ms) + laps * sizeof lap[0]);
}

static void load_timer(uint32_t *set, uint32_t *left, uint32_t *end)
{
	struct saved_timer v;
	if (kv_get(KV_TIMER, &v, sizeof v) != sizeof v || !v.set_ms)
		return;
	uint32_t rtc = pmic_rtc_read();
	*set = v.set_ms;
	*left = v.end_rtc && v.end_rtc <= rtc ? v.set_ms : v.left_ms;
	if (v.end_rtc > rtc)
		*end = time_ms() + (v.end_rtc - rtc) * 1000;
}

static unsigned load_stopwatch(uint32_t *acc, bool *running, uint32_t *start, uint32_t *lap)
{
	static struct saved_stopwatch v;
	int n = kv_get(KV_STOPWATCH, &v, sizeof v);
	unsigned head = offsetof(struct saved_stopwatch, lap_ms);
	if (n < (int)head || v.laps > LAPS || n != (int)(head + v.laps * sizeof v.lap_ms[0]))
		return 0;
	uint32_t rtc = pmic_rtc_read();
	*acc = v.acc_ms + (v.running && rtc > v.start_rtc ? (rtc - v.start_rtc) * 1000 : 0);
	*running = v.running;
	*start = time_ms();
	memcpy(lap, v.lap_ms, v.laps * sizeof lap[0]);
	return v.laps;
}

/* The clock set by hand: the time of day in minutes, keeping the date,
 * or a date, keeping the time. */
static void set_clock(int kind, int v)
{
	struct civil c;
	civil_from_epoch(pmic_rtc_read(), &c);
	if (kind == UI_EVENT_TIME_SET) {
		if (v < 0 || v >= 24 * 60)
			return;
		c.hour = (uint32_t)v / 60;
		c.minute = (uint32_t)v % 60;
		c.second = 0;
	} else {
		uint32_t day = v & 31, month = v >> 5 & 15, year = 2000 + (v >> 9 & 127);
		bool leap = year % 4 == 0 && (year % 100 || year % 400 == 0);
		uint32_t days = month == 2 ? 28u + leap : 30 + ((month + month / 8) & 1);
		if (v < 0 || month < 1 || month > 12 || day < 1 || year > 2099)
			return;
		c.day = day > days ? days : day;
		c.month = month;
		c.year = year;
	}
	pmic_rtc_write(epoch_from_civil(&c));
	log_printf("clock set to %04lu-%02lu-%02lu %02lu:%02lu\n", c.year, c.month, c.day, c.hour, c.minute);
}

/* A setting set to a value from the UI, kept to the steps it offers. */
static void setting_value(int s, int v)
{
	if (s == SETTING_NIGHTSTAND_BRIGHTNESS && v >= 10 && v <= 100) {
		setting_set(SETTING_NIGHTSTAND_BRIGHTNESS, v / 10 * 10);
		set_brightness();
	} else if (s == SETTING_NIGHTSTAND_DELAY && v >= 0 && v <= 30) {
		setting_set(SETTING_NIGHTSTAND_DELAY, v / 5 * 5);
	} else if (s == SETTING_BURN_IN && v >= 0 && v <= 3) {
		setting_set(SETTING_BURN_IN, v);
	}
	show_settings();
}

static void show_alarms(void)
{
	struct ui_alarm list[ALARMS];
	unsigned n = alarm_count();
	for (unsigned i = 0; i < n; i++) {
		const struct alarm *a = alarm_get(i);
		list[i] = (struct ui_alarm){ a->hour, a->minute, a->days, a->on };
	}
	ui_set_alarms(list, n);
}

/* A change from the alarm app: an alarm edited or deleted. */
static void alarm_edited(const struct ui_event *e)
{
	if (e->kind == UI_EVENT_ALARM_DELETE) {
		if (e->value >= 0)
			alarm_delete((unsigned)e->value);
	} else if (e->alarm.hour < 24 && e->alarm.minute < 60) {
		struct alarm a = { e->alarm.hour, e->alarm.minute, (uint8_t)(e->alarm.days & 0x7f), e->alarm.on };
		if (!alarm_set(e->value >= 0 ? (unsigned)e->value : ALARMS, &a))
			log_printf("alarm: no room\n");
	}
	show_alarms();
}

static void vibrate(const char *how)
{
	static const uint16_t normal[] = { 200 }, strong[] = { 250, 150, 250 },
			      ringtone[] = { 600, 400, 600, 400, 600, 400, 600 };
	if (!strcmp(how, "none"))
		return;
	if (!strcmp(how, "normal"))
		haptics_play(normal, ARRAY_SIZE(normal));
	else if (!strcmp(how, "ringtone"))
		haptics_play(ringtone, ARRAY_SIZE(ringtone));
	else
		haptics_play(strong, ARRAY_SIZE(strong));
}

static void show_weather(void)
{
	const struct weather *w = weather_get();
	int32_t ids[WEATHER_DAYS], lo[WEATHER_DAYS], hi[WEATHER_DAYS];
	uint32_t today = pmic_rtc_read() / 86400, day0 = w->time / 86400;
	unsigned n = 0;
	for (uint32_t i = today >= day0 ? today - day0 : WEATHER_DAYS; i < w->days; i++, n++) {
		ids[n] = w->id[i];
		lo[n] = (int32_t)w->min[i] - 273;
		hi[n] = (int32_t)w->max[i] - 273;
	}
	ui_set_weather(w->city, strlen(w->city), n, ids, lo, hi);
}

void main(void)
{
	cpu_clock_init();
	mmu_init();
	gic_init();
	timer_init();
	dload_clear();
	int scm = scm_wdog_debug_disable();
	wdt_arm(3000, 4000, wdt_bark_handler);
	irq_enable();
	log_init();
	log_printf("asteroid-embedded %s up, timer %lu Hz, scm %d\n", build_id, timer_hz(), scm);
	spmi_init();
	pmic_init();
	if (rpm_init()) {
		rpm_bus_votes();
		rpm_sleep_votes();
	}
	log_printf("spmi arbiter %08lx, %s\n", spmi_version(), pmic_pon_reason());
	bool have_display = display_init();
	if (have_display)
		fbcon_init();
	const char *prev = log_previous_record();
	if (prev) {
		log_puts("previous boot ended with:\n");
		log_puts(prev);
		log_puts("\n");
	} else
		log_printf("no fault record from the previous boot\n");

	pmic_charger_configure();
	bms_init();
	spm_init();
	have_gpu = gpu_init() && gpu_selftest();
	bool have_ui = have_display && ui_init((void *)UI_HEAP_BASE, UI_HEAP_SIZE, layers, UI_LAYER_STRIDE / 4, fb.stride,
					       fb.width, fb.height);
	if (have_ui) {
		stack_init(fb.width, fb.height);
		for (unsigned p = 0; p < UI_PANELS; p++)
			dcache_clean_range(layers + p * (UI_LAYER_STRIDE / 4), fb.stride * fb.height * 4);
	}
	log_printf("ui: %s\n", have_ui ? "ready" : "not started");
	if (have_display && !have_ui)
		log_set_sink(sink_fbcon);
	tlmm_irq_init();
	pmic_power_key_irq_init();
	pmic_usb_irq_init();
	bool have_touch = touch_init();
	bool have_haptics = haptics_init();
	bool have_hub = hub_init();
	console_init();
	usb_init("asteroid-sturgeon");
	if (have_display)
		display_set_idle_hook(usb_poll);
	log_printf("rtc %lu, battery %lu mV, %s\n", pmic_rtc_read(), pmic_battery_mv(), pmic_charger_status());

	uint32_t t0 = time_ms(), last = 0, last_clock = 0, frames0 = display_frames();
	uint32_t ui_frames = 0, ui_us = 0, ui_repaints = 0, ui_repaint_us = 0;
	bool sleep_request = false, wake_request = false, gpu_off = false;
	bool charger_wake = false;
	bool key_was = false, awake = true, woke = false, ringing = false, ring_timer = false, sw_running = false;
	static uint32_t lap_ms[LAPS];
	uint32_t alarm_t = 0, ring_t = 0, timer_set = 300000, timer_left = 300000, timer_end = 0, sw_acc = 0, sw_start = 0;
	load_timer(&timer_set, &timer_left, &timer_end);
	unsigned laps = load_stopwatch(&sw_acc, &sw_running, &sw_start, lap_ms);
	if (have_ui)
		ui_set_laps(lap_ms, laps);
	uint32_t shown_timer = 1, shown_sw = 1, shown_step = UINT32_MAX;
	int32_t passkey = -1;
	uint32_t notify_seen = 0, bt_start_t = time_ms() + BT_START_MS;
	uint32_t shown_seq = 0, newest_seq = 0, media_seen = 0, weather_seen = 0, weather_day = 0;
	uint32_t key_down_t = 0, last_activity = time_ms(), asleep_t = 0, asleep_ms = 0, wakeups = 0;
	struct touch t;
	int down_x = 0, down_y = 0;
	uint32_t down_t = 0;
	if (have_display)
		fbcon_set_overlay(true);
	for (;;) {
		uint32_t now = time_ms();
		wdt_feed();
		usb_poll();
		bt_poll();
		screenshot_poll();
		weather_poll();
		if (!awake)
			wakeups++;
		if (bt_start_t && now - bt_start_t < 0x80000000u) {
			bt_start_t = 0;
			weather_load();
			if (setting_get(SETTING_BLUETOOTH))
				bluetooth_start();
			if (have_display)
				set_brightness();
			if (have_ui) {
				show_settings();
				ui_set_build(build_id);
				show_alarms();
			}
		}
		pmic_charger_poll();
		bms_poll();
		if (have_hub) {
			hub_poll();
			if (hub_tilted() && !awake && setting_get(SETTING_TILT))
				wake_request = true;
		}
		if (have_haptics)
			haptics_poll();
		while (have_touch && touch_poll(&t)) {
			if (t.event == TOUCH_PALM) {
				log_printf("touch: palm\n");
				sleep_request |= awake;
				continue;
			}
			if (!awake) {
				if (t.event == TOUCH_DOWN && setting_get(SETTING_TAP))
					wake_request = true;
				continue;
			}
			last_activity = now;
			if (have_ui)
				stack_touch(t.event == TOUCH_DOWN ? UI_TOUCH_DOWN : t.event == TOUCH_UP ? UI_TOUCH_UP : UI_TOUCH_MOVE, t.x,
					    t.y, now);
			if (t.event == TOUCH_DOWN) {
				down_x = t.x;
				down_y = t.y;
				down_t = now;
			} else if (t.event == TOUCH_UP) {
				int dx = t.x - down_x, dy = t.y - down_y;
				const char *g = "tap";
				if (dx * dx + dy * dy > 40 * 40)
					g = (dx * dx > dy * dy) ? (dx > 0 ? "swipe right" : "swipe left") : (dy > 0 ? "swipe down" : "swipe up");
				log_printf("gesture: %s (%d,%d -> %d,%d in %lu ms)\n", g, down_x, down_y, t.x, t.y, now - down_t);
			}
			dot_x = t.down ? t.x : -1;
			dot_y = t.y;
		}
		pmic_power_key_irq();
		/* A cable plugged in shows the screen for CHARGER_WAKE_MS, as
		 * mce does. */
		if (pmic_usb_irq() && pmic_usb_present()) {
			log_printf("usb: cable plugged in\n");
			if (!awake)
				charger_wake = true;
			else if (now - last_activity > SLEEP_AFTER_MS - CHARGER_WAKE_MS)
				last_activity = now - (SLEEP_AFTER_MS - CHARGER_WAKE_MS);
		}
		bool key = pmic_power_key();
		if (key != key_was) {
			key_was = key;
			if (key) {
				key_down_t = now;
			} else if (now - key_down_t >= 3000) {
				log_printf("rebooting on power key\n");
				reboot();
			} else if (awake) {
				sleep_request = true;
			} else {
				wake_request = true;
			}
		}
		if (bt_passkey() != passkey) {
			passkey = bt_passkey();
			if (have_ui)
				ui_set_passkey(passkey);
			if (passkey >= 0)
				wake_request = !awake;
			last_activity = now;
		}
		if (notify_serial() != notify_seen) {
			notify_seen = notify_serial();
			unsigned n = notify_count();
			if (have_ui)
				stack_pending(n > 0);
			const struct notification *top = n ? notify_get(n - 1) : NULL;
			if ((top ? top->seq : 0) != shown_seq && have_haptics)
				haptics_play(NULL, 0);
			if (have_ui) {
				ui_clear_notifications();
				for (unsigned i = n; i--;) {
					const struct notification *e = notify_get(i);
					ui_add_notification(e->id, e->app, strlen(e->app), e->summary, strlen(e->summary), e->body,
							    strlen(e->body), e->icon, strlen(e->icon), e->time);
				}
			}
			if (top && top->seq > newest_seq) {
				newest_seq = top->seq;
				if (have_ui)
					ui_show_notification(top->id);
				if (have_haptics && setting_get(SETTING_VIBRATION))
					vibrate(top->vibrate);
				wake_request = !awake;
				last_activity = now;
			}
			shown_seq = top ? top->seq : 0;
		}
		if (have_ui && media_serial() != media_seen) {
			media_seen = media_serial();
			const struct media *m = media_get();
			ui_set_media(m->title, strlen(m->title), m->artist, strlen(m->artist), m->album, strlen(m->album),
				     m->playing, m->volume);
		}
		if (now - alarm_t >= 1000) {
			alarm_t = now;
			notify_tick(pmic_rtc_read());
			if (alarm_check(pmic_rtc_read()) && have_ui) {
				log_printf("alarm: ringing\n");
				ringing = true;
				ring_timer = false;
				ring_t = now;
				ui_set_ringing(true, false);
				show_alarms();
				wake_request = !awake;
			}
		}
		if (timer_end && (int32_t)(now - timer_end) >= 0 && have_ui) {
			log_printf("timer: done\n");
			timer_end = 0;
			timer_left = timer_set;
			ringing = ring_timer = true;
			ring_t = now;
			ui_set_ringing(true, true);
			wake_request = !awake;
		}
		for (struct ui_event e; have_ui && ui_next_event(&e);) {
			int a = e.value;
			last_activity = now;
			switch (e.kind) {
			case UI_EVENT_SETTING:
				setting_toggled(a);
				break;
			case UI_EVENT_APP:
				open_app(a);
				break;
			case UI_EVENT_MEDIA:
				media_command((unsigned)a);
				break;
			case UI_EVENT_DISMISS:
				notify_dismiss(a);
				break;
			case UI_EVENT_DISMISS_ALL:
				notify_dismiss_all();
				break;
			case UI_EVENT_TIME_SET:
			case UI_EVENT_DATE_SET:
				set_clock(e.kind, a);
				last_clock = 0;
				break;
			case UI_EVENT_SETTING_VALUE:
				setting_value(a >> 8, a & 0xff);
				break;
			case UI_EVENT_SNOOZE:
				notify_snooze(a, pmic_rtc_read() + (e.alarm.hour * 60 + e.alarm.minute) * 60);
				break;
			case UI_EVENT_TIMER_SET:
				if (!timer_end && a >= 1 && a <= 86399) {
					timer_set = timer_left = (uint32_t)a * 1000;
					shown_timer = UINT32_MAX;
					save_timer(timer_set, timer_left, timer_end, now);
				}
				break;
			case UI_EVENT_BRIGHTNESS:
				if (a >= 10 && a <= 100) {
					setting_set(SETTING_BRIGHTNESS, a);
					set_brightness();
					show_settings();
				}
				break;
			case UI_EVENT_ALARM_EDIT:
			case UI_EVENT_ALARM_DELETE:
				alarm_edited(&e);
				break;
			case UI_EVENT_ALARM_RING:
				if (!ringing)
					break;
				if (a == UI_ALARM_SNOOZE && !ring_timer)
					alarm_snooze(pmic_rtc_read());
				log_printf("alarm: %s\n", a == UI_ALARM_SNOOZE ? "snoozed" : "dismissed");
				ringing = false;
				break;
			case UI_EVENT_CLOCK:
				if (a == UI_TIMER_START && timer_end) {
					timer_left = timer_end - now;
					timer_end = 0;
				} else if (a == UI_TIMER_START && timer_left) {
					timer_end = now + timer_left;
				} else if (a == UI_TIMER_RESET) {
					timer_end = 0;
					timer_left = timer_set;
				} else if (a >= UI_TIMER_MINUTE_UP && a <= UI_TIMER_SECONDS_DOWN && !timer_end) {
					int32_t step = a <= UI_TIMER_MINUTE_DOWN ? 60000 : 10000, t = (int32_t)timer_set;
					t += a == UI_TIMER_MINUTE_UP || a == UI_TIMER_SECONDS_UP ? step : -step;
					timer_set = timer_left = (uint32_t)(t < 10000 ? 10000 : t > 5940000 ? 5940000 : t);
				} else if (a == UI_STOPWATCH_START) {
					if (sw_running)
						sw_acc += now - sw_start;
					else
						sw_start = now;
					sw_running = !sw_running;
				} else if (a == UI_STOPWATCH_RESET) {
					sw_acc = 0;
					sw_running = false;
					laps = 0;
					ui_set_laps(lap_ms, 0);
				} else if (a == UI_STOPWATCH_LAP && sw_running && laps < ARRAY_SIZE(lap_ms)) {
					memmove(lap_ms + 1, lap_ms, laps++ * sizeof lap_ms[0]);
					lap_ms[0] = sw_acc + now - sw_start;
					ui_set_laps(lap_ms, laps);
				}
				if (a >= UI_STOPWATCH_START)
					save_stopwatch(sw_acc, sw_running, sw_start, now, lap_ms, laps);
				else
					save_timer(timer_set, timer_left, timer_end, now);
				shown_timer = shown_sw = UINT32_MAX;
				break;
			}
		}
		if (have_ui && awake) {
			uint32_t t = timer_end ? timer_end - now : timer_left, sw = sw_acc + (sw_running ? now - sw_start : 0);
			if (t / 100 != shown_timer) {
				shown_timer = t / 100;
				ui_set_timer(t, timer_set, timer_end);
			}
			if (sw / 100 != shown_sw) {
				shown_sw = sw / 100;
				ui_set_stopwatch(sw, sw_running);
			}
		}
		/* Unanswered, an alarm snoozes after RING_MS, as the alarm
		 * presenter's autoSnoozeTimer does; a timer just stops. */
		if (ringing && now - ring_t >= RING_MS) {
			if (!ring_timer)
				alarm_snooze(pmic_rtc_read());
			log_printf("alarm: %s\n", ring_timer ? "stopped ringing" : "snoozed");
			ringing = false;
		}
		if (ringing) {
			static const uint16_t ring[] = { 500, 500 };
			last_activity = now;
			if (have_haptics && !haptics_busy())
				haptics_play(ring, ARRAY_SIZE(ring));
		} else if (have_ui && ring_t) {
			ring_t = 0;
			ui_set_ringing(false, false);
			if (have_haptics)
				haptics_play(NULL, 0);
		}
		if (have_ui && weather_serial() != weather_seen) {
			weather_seen = weather_serial();
			show_weather();
		}
		settings_poll(now);
		if (console_settings) {
			console_settings = false;
			if (have_display)
				set_brightness();
			if (have_ui)
				show_settings();
		}
		if (console_sleep || console_wake) {
			sleep_request |= console_sleep && awake;
			wake_request |= console_wake && !awake;
			console_sleep = console_wake = false;
		}
		/* The short charger wake only when nothing else wakes the watch. */
		if (charger_wake && !awake && !wake_request)
			wake_request = true;
		else
			charger_wake = false;
		if (have_display && awake && (sleep_request || now - last_activity >= SLEEP_AFTER_MS)) {
			if (ringing) {
				if (!ring_timer)
					alarm_snooze(pmic_rtc_read());
				log_printf("alarm: %s\n", ring_timer ? "dismissed" : "snoozed");
				ringing = false;
			}
			settings_flush();
			ambient = have_ui && setting_get(nightstand_on() ? SETTING_NIGHTSTAND_AOD : SETTING_AOD);
			if (!ambient)
				display_sleep();
			if (have_ui) {
				stack_sleep(now);
				ui_hide_notification();
			}
			if (have_gpu) {
				gpu_suspend();
				gpu_off = true;
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
		} else if (have_display && !awake && wake_request) {
			cpu_clock_slow(false);
			if (gpu_off) {
				uint32_t g0 = time_us();
				have_gpu = gpu_init();
				gpu_off = false;
				log_printf("gpu: resumed in %lu us\n", time_us() - g0);
			}
			timer_set_period(1);
			if (ambient) {
				ui_set_ambient(false);
				display_idle(false);
				set_brightness();
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
			last_clock = 0;
			woke = true;
			if (have_ui)
				stack_wake(now);
			log_printf("wake\n");
		}
		sleep_request = wake_request = false;
		if (ambient && !awake && pmic_rtc_read() / 60 != ambient_minute) {
			cpu_clock_slow(false);
			ambient_draw();
			cpu_clock_slow(true);
		}
		/* Awake, a frame is drawn when the UI repainted, the stack moves
		 * or animates, or the FlatMesh takes its next step: every 90 ms,
		 * as qml-asteroid's FlatMesh timer. In between the CPU waits for
		 * an interrupt, at the latest the next 1 ms tick. */
		if (have_display && awake) {
			uint32_t *buf = display_backbuffer(), step = (now - t0) / MESH_STEP_MS;
			bool frame = !have_ui || woke || step != shown_step;
			if (have_ui) {
				uint32_t u0 = time_us(), x, y, w, h;
				bool repainted = false;
				int sx, sy;
				stack_update(now);
				if (stack_moving())
					last_activity = now;
				ui_tick();
				uint32_t damage[UI_PANELS][4];
				bool damaged[UI_PANELS] = { false };
				for (unsigned p = 0; p < UI_PANELS; p++)
					if ((p != UI_PANEL_OVERLAY || !stack_overlay_frozen()) &&
					    ui_update(p, &damage[p][0], &damage[p][1], &damage[p][2], &damage[p][3]))
						damaged[p] = repainted = true;
				if (repainted)
					paint_finish();
				for (unsigned p = 0; p < UI_PANELS; p++) {
					if (!damaged[p])
						continue;
					uint32_t *layer = layers + p * (UI_LAYER_STRIDE / 4);
					x = damage[p][0], y = damage[p][1], w = damage[p][2], h = damage[p][3];
					stack_damage(p, x, y, w, h);
					if (have_gpu)
						for (uint32_t r = y; r < y + h; r++)
							dcache_clean_range(layer + r * fb.stride + x, w * 4);
				}
				frame |= repainted || stack_animating(now);
				if (repainted) {
					ui_repaints++;
					ui_repaint_us += time_us() - u0;
				}
				ui_us += time_us() - u0;
				ui_frames++;
				stack_offset(&sx, &sy);
				flatmesh_view(1.1f, (float)sx / fb.width * 0.05f, (float)sy / fb.height * 0.05f, 1.0f - stack_dim());
				mesh_colors(stack_launcher());
			}
			if (frame) {
				shown_step = step;
				if (have_gpu) {
					gpu_flatmesh(buf, step * MESH_STEP_MS, have_ui);
				} else {
					flatmesh_render(buf, step * MESH_STEP_MS);
					if (have_ui)
						ui_render(buf, fb.stride, 0, 0);
				}
				if (!have_ui) {
					draw_dot(buf);
					fbcon_draw(buf);
				}
				display_flip();
				/* The indicators pop out once the first frame is on the panel. */
				if (woke && have_ui)
					stack_indicators();
				woke = false;
			} else {
				wfi();
			}
		}
		if (have_ui && awake && (!last_clock || now - last_clock >= 1000)) {
			last_clock = now;
			struct civil c;
			uint32_t rtc = pmic_rtc_read();
			civil_from_epoch(rtc, &c);
			ui_set_clock(c.hour, c.minute, c.second, c.weekday, c.day, c.month, c.year, rtc);
			if (rtc / 86400 != weather_day) {
				weather_day = rtc / 86400;
				show_weather();
			}
			ui_set_battery(bms_percent(), pmic_charging());
			ui_set_connected(bt_host_connected(), bt_store_bonded());
			static char addr_text[18];
			uint8_t addr[6];
			if (!addr_text[0] && !bt_addr_load(addr))
				snprintf(addr_text, sizeof addr_text, "%02X:%02X:%02X:%02X:%02X:%02X", addr[5], addr[4], addr[3],
					 addr[2], addr[1], addr[0]);
			ui_set_info(addr_text, now / 1000);
			static bool ns;
			if (nightstand_on() != ns) {
				ns = !ns;
				set_brightness();
			}
			ui_set_face(setting_get(SETTING_WATCHFACE), setting_get(SETTING_NIGHTSTAND), ns);
		}
		if (!awake && now - last >= ASLEEP_STATS_MS) {
			last = now;
			uint32_t pc, pcc, pcb;
			spm_stats(&pc, &pcc, &pcb);
			log_printf("power: asleep, %lu wakeups, %lu/%lu/%lu collapses, %ld mA average\n", wakeups, pc, pcc, pcb,
				   pmic_battery_avg_ua() / 1000);
			wakeups = 0;
		}
		if (awake && now - last >= 1000) {
			last = now;
			uint32_t rx, tx, att, terr;
			usb_stats(&rx, &tx);
			touch_stats(&att, &terr);
			log_printf("tick %lu, %lu fps, rtc %lu, battery %lu mV, %s, usb %lu/%lu, touch %lu/%lu\n", (now - t0) / 1000,
				   display_frames() - frames0, pmic_rtc_read(), pmic_battery_mv(), pmic_charger_status(), rx, tx, att, terr);
			frames0 = display_frames();
			uint32_t slept = asleep_ms + (awake ? 0 : now - asleep_t);
			uint32_t pc, pcc, pcb;
			spm_stats(&pc, &pcc, &pcb);
			log_printf("power: %s, asleep %lu%% of the last second, %lu wakeups, %lu/%lu/%lu collapses, battery %ld mA, %ld mA average\n",
				   awake ? "awake" : "asleep", slept > 1000 ? 100 : slept / 10, wakeups, pc, pcc, pcb,
				   pmic_battery_ua() / 1000, pmic_battery_avg_ua() / 1000);
			asleep_ms = 0;
			if (!awake)
				asleep_t = now;
			wakeups = 0;
			if (gpu_frames)
				log_printf("gpu: %lu frames, %lu us submit to idle, %lu us vertices\n", gpu_frames,
					   gpu_us / gpu_frames, gpu_vert_us / gpu_frames);
			gpu_frames = gpu_us = gpu_vert_us = 0;
			if (ui_frames)
				log_printf("ui: %lu of %lu frames repainted, update %lu us avg, %lu us per repaint\n", ui_repaints,
					   ui_frames, ui_us / ui_frames, ui_repaints ? ui_repaint_us / ui_repaints : 0);
			ui_frames = ui_us = ui_repaints = ui_repaint_us = 0;
#if defined(TEST_HANG)
			if (now - t0 >= 2000) {
				log_printf("test: hanging the main loop\n");
				for (;;)
					;
			}
#elif defined(TEST_ABORT)
			if (now - t0 >= 2000) {
				log_printf("test: data abort\n");
				*(volatile uint32_t *)0xe0000000u = 1;
			}
#endif
		}
#if REBOOT_AFTER_MS > 0
		if (now - t0 >= REBOOT_AFTER_MS) {
			log_printf("rebooting, %lu ticks\n", timer_ticks());
			reboot();
		}
#endif
		if ((!have_display || !awake) && !haptics_busy() && !screenshot_busy() && timer_ticks()) {
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
				uint32_t bt = bt_idle_ms(), lpm = bt_lpm_next_ms(), stats = ASLEEP_STATS_MS - (time_ms() - last);
				next = stats > ASLEEP_STATS_MS ? 1 : stats;
				if (bt < next)
					next = bt;
				if (lpm < next)
					next = lpm;
				if (have_hub && !hub_can_sleep() && next > 50)
					next = 50;
				uint32_t rtc = pmic_rtc_read(), minute = (60 - rtc % 60) * 1000, alarm = alarm_next_s(rtc);
				if (ambient && minute < next)
					next = minute;
				if (alarm < next / 1000)
					next = alarm ? alarm * 1000 : 1;
				if (timer_end && timer_end - time_ms() < next)
					next = (int32_t)(timer_end - time_ms()) > 0 ? timer_end - time_ms() : 1;
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
	}
}
