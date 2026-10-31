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
#include "sensors/steps.h"
#include "media/media.h"
#include "weather/weather.h"
#include "screenshot/screenshot.h"
#include "storage/storage.h"
#include "alarm.h"
#include "clockapps.h"
#include "power.h"
#include "config.h"

#define MESH_STEP_MS 90
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

static bool console_settings;

void settings_changed(void)
{
	console_settings = true;
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
	ui_set_face(setting_get(SETTING_WATCHFACE), setting_get(SETTING_NIGHTSTAND), power_nightstand());
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
		power_set_brightness();
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
		power_set_brightness();
	} else if (s >= 0 && s < SETTINGS) {
		setting_set((enum setting)s, !setting_get((enum setting)s));
	}
	show_settings();
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
		power_set_brightness();
	} else if (s == SETTING_NIGHTSTAND_DELAY && v >= 0 && v <= 30) {
		setting_set(SETTING_NIGHTSTAND_DELAY, v / 5 * 5);
	} else if (s == SETTING_BURN_IN && v >= 0 && v <= 3) {
		setting_set(SETTING_BURN_IN, v);
	}
	show_settings();
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
		stack_init(fb.width, fb.height, layers, UI_LAYER_STRIDE / 4);
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
	bool have_hub = hub_init(), steps_awake = false;
	power_init(have_display, have_ui, have_hub);
	console_init();
	usb_init("asteroid-" BOARD_NAME);
	if (have_display)
		display_set_idle_hook(usb_poll);
	log_printf("rtc %lu, battery %lu mV, %s\n", pmic_rtc_read(), pmic_battery_mv(), pmic_charger_status());

	uint32_t t0 = time_ms(), last = 0, last_clock = 0, frames0 = display_frames();
	uint32_t ui_frames = 0, ui_us = 0, ui_repaints = 0, ui_repaint_us = 0;
	bool gpu_off = false;
	bool key_was = false, woke = false, wake_touch = false;
	uint32_t alarm_t = 0, shown_step = UINT32_MAX;
	clockapps_init(have_ui, have_haptics);
	int32_t passkey = -1;
	uint32_t notify_seen = 0, bt_start_t = time_ms() + BT_START_MS;
	uint32_t shown_seq = 0, newest_seq = 0, media_seen = 0, weather_seen = 0, weather_day = 0;
	uint32_t key_down_t = 0;
	struct touch t;
	int down_x = 0, down_y = 0;
	uint32_t down_t = 0;
	if (have_display)
		fbcon_set_overlay(true);
	for (;;) {
		uint32_t now = time_ms();
		wdt_feed();
		usb_poll();
		console_poll();
		bt_poll();
		screenshot_poll();
		weather_poll();
		if (bt_start_t && now - bt_start_t < 0x80000000u) {
			bt_start_t = 0;
			weather_load();
			if (have_hub)
				steps_init();
			if (setting_get(SETTING_BLUETOOTH))
				bluetooth_start();
			if (have_display)
				power_set_brightness();
			if (have_ui) {
				show_settings();
				ui_set_build(build_id);
				clockapps_show_alarms();
			}
		}
		pmic_charger_poll();
		bms_poll();
		if (have_hub) {
			hub_poll();
			if (hub_tilted() && !power_awake() && setting_get(SETTING_TILT))
				power_request(false);
			if (steps_awake && !power_awake())
				steps_save();
			steps_awake = power_awake();
		}
		if (have_haptics)
			haptics_poll();
		while (have_touch && touch_poll(&t)) {
			if (t.event == TOUCH_PALM) {
				log_printf("touch: palm\n");
				if (power_awake())
					power_request(true);
				continue;
			}
			if (!power_awake()) {
				if (t.event == TOUCH_DOWN && setting_get(SETTING_TAP)) {
					power_request(false);
					wake_touch = true;
				}
				continue;
			}
			/* The touch that woke the watch is not a touch on the UI. */
			if (wake_touch) {
				wake_touch = t.event != TOUCH_UP;
				continue;
			}
			power_activity(now);
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
		if (pmic_usb_irq() && pmic_usb_present()) {
			log_printf("usb: cable plugged in\n");
			power_charger_plugged(now);
		}
		bool key = pmic_power_key();
		if (key != key_was) {
			key_was = key;
			if (key) {
				key_down_t = now;
			} else if (now - key_down_t >= 3000) {
				log_printf("rebooting on power key\n");
				reboot();
			} else {
				power_request(power_awake());
			}
		}
		if (bt_passkey() != passkey) {
			passkey = bt_passkey();
			if (have_ui)
				ui_set_passkey(passkey);
			if (passkey >= 0 && !power_awake())
				power_request(false);
			power_activity(now);
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
				if (!power_awake())
					power_request(false);
				power_activity(now);
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
		}
		if (clockapps_tick(now) && !power_awake())
			power_request(false);
		if (clockapps_ringing())
			power_activity(now);
		for (struct ui_event e; have_ui && ui_next_event(&e);) {
			int a = e.value;
			power_activity(now);
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
				setting_value(a, e.arg);
				break;
			case UI_EVENT_SNOOZE:
				notify_snooze(a, pmic_rtc_read() + (e.alarm.hour * 60 + e.alarm.minute) * 60);
				break;
			case UI_EVENT_BRIGHTNESS:
				if (a >= 10 && a <= 100) {
					setting_set(SETTING_BRIGHTNESS, a);
					power_set_brightness();
					show_settings();
				}
				break;
			default:
				clockapps_event(&e, now);
				break;
			}
		}
		if (power_awake())
			clockapps_refresh(now);
		if (have_ui && weather_serial() != weather_seen) {
			weather_seen = weather_serial();
			show_weather();
		}
		settings_poll(now);
		if (console_settings) {
			console_settings = false;
			if (have_display)
				power_set_brightness();
			if (have_ui)
				show_settings();
		}
		switch (power_update(&now)) {
		case POWER_SLEPT:
			if (have_gpu) {
				gpu_suspend();
				gpu_off = true;
			}
			break;
		case POWER_WOKE:
			if (gpu_off) {
				uint32_t g0 = time_us();
				have_gpu = gpu_init();
				gpu_off = false;
				log_printf("gpu: resumed in %lu us\n", time_us() - g0);
			}
			last_clock = 0;
			woke = true;
			break;
		case POWER_NONE:
			break;
		}
		/* Awake, a frame is drawn when the UI repainted, the stack moves
		 * or animates, or the FlatMesh takes its next step: every 90 ms,
		 * as qml-asteroid's FlatMesh timer. In between the CPU waits for
		 * an interrupt, at the latest the next 1 ms tick. */
		if (have_display && power_awake()) {
			uint32_t *buf = display_backbuffer(), step = (now - t0) / MESH_STEP_MS;
			bool frame = !have_ui || woke || step != shown_step;
			if (have_ui) {
				uint32_t u0 = time_us(), x, y, w, h;
				bool repainted = false;
				int sx, sy;
				stack_update(now);
				if (stack_moving())
					power_activity(now);
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
		if (have_ui && power_awake() && (!last_clock || now - last_clock >= 1000)) {
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
			if (have_hub)
				steps_poll();
			power_tick();
			ui_set_face(setting_get(SETTING_WATCHFACE), setting_get(SETTING_NIGHTSTAND), power_nightstand());
		}
		bool awake = power_awake();
		if (!awake && now - last >= ASLEEP_STATS_MS) {
			last = now;
			uint32_t pc, pcc, pcb;
			spm_stats(&pc, &pcc, &pcb);
			log_printf("power: asleep, %lu wakeups, %lu/%lu/%lu collapses, %ld mA average\n", power_wakeups(), pc, pcc,
				   pcb, pmic_battery_avg_ua() / 1000);
		}
		if (awake && now - last >= 1000) {
			last = now;
			uint32_t rx, tx, att, terr;
			usb_stats(&rx, &tx);
			touch_stats(&att, &terr);
			log_printf("tick %lu, %lu fps, rtc %lu, battery %lu mV, %s, usb %lu/%lu, touch %lu/%lu\n", (now - t0) / 1000,
				   display_frames() - frames0, pmic_rtc_read(), pmic_battery_mv(), pmic_charger_status(), rx, tx, att, terr);
			frames0 = display_frames();
			uint32_t slept = power_slept_ms(now);
			uint32_t pc, pcc, pcb;
			spm_stats(&pc, &pcc, &pcb);
			log_printf("power: %s, asleep %lu%% of the last second, %lu wakeups, %lu/%lu/%lu collapses, battery %ld mA, %ld mA average\n",
				   awake ? "awake" : "asleep", slept > 1000 ? 100 : slept / 10, power_wakeups(), pc, pcc, pcb,
				   pmic_battery_ua() / 1000, pmic_battery_avg_ua() / 1000);
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
		uint32_t app_next = 1000;
		if (!awake) {
			uint32_t stats = ASLEEP_STATS_MS - (time_ms() - last);
			app_next = stats > ASLEEP_STATS_MS ? 1 : stats;
			uint32_t app = clockapps_next_ms(time_ms());
			if (app < app_next)
				app_next = app;
		}
		power_idle(app_next);
	}
}
