/* Bring-up and the main loop. The display round: console on the firmware's
 * own buffers, then the FlatMesh background at the panel's pace with the
 * console as an overlay, then brightness and panel sleep/wake, then a
 * reboot so the bootloader is back in fastboot for the next image. A
 * build-time TEST hangs or faults on purpose to exercise the crash path. */
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
#include "gitrev.h"
#include "config.h"

const char build_id[] = GITREV;

#define SLEEP_AFTER_MS 10000
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

static void gpu_flatmesh(uint32_t *buf, uint32_t t_ms, bool with_layer)
{
	static float *vbo;
	if (!vbo)
		vbo = gpu_alloc(FLATMESH_GPU_VERTICES * 32, 64);
	uint32_t t0 = time_us();
	unsigned n = flatmesh_vertices_gpu(vbo, t_ms);
	struct gpu_target t = { (uint32_t)(uintptr_t)buf, fb.width, fb.height, fb.stride * 4, false };
	dcache_invalidate_range(buf, fb.stride * fb.height * 4);
	uint32_t t1 = time_us();
	gpu_draw_begin(&t);
	gpu_draw_program(&shader_c_vs, &shader_c_fs);
	gpu_draw_triangles((uint32_t)(uintptr_t)vbo, 32, 2, (const uint8_t[]){4, 4}, n);
	if (with_layer)
		stack_compose();
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

static volatile bool console_sleep, console_wake;

void power_request(bool sleep)
{
	if (sleep)
		console_sleep = true;
	else
		console_wake = true;
}

/* The always-on face: shown while asleep unless turned off or the palm
 * covered the screen, drawn on the CPU so the GPU can stay off, with the
 * panel in idle mode. */
static bool ambient;
static uint32_t ambient_minute;

static void ambient_draw(void)
{
	struct civil c;
	uint32_t rtc = pmic_rtc_read(), x, y, w, h, *buf = display_backbuffer();
	civil_from_epoch(rtc, &c);
	ambient_minute = rtc / 60;
	ui_set_clock(c.hour, c.minute, c.second, c.weekday, c.day, c.month);
	ui_set_battery(bms_percent(), pmic_charging());
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
	memset(buf, 0, fb.stride * fb.height * 4);
	ui_render(buf, fb.stride);
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

static void show_settings(void)
{
	ui_set_settings(setting_get(SETTING_BRIGHTNESS), setting_get(SETTING_BLUETOOTH), setting_get(SETTING_VIBRATION),
			setting_get(SETTING_AOD), setting_get(SETTING_CLOCK12));
}

static void set_brightness(void)
{
	display_set_brightness((uint8_t)(setting_get(SETTING_BRIGHTNESS) * 255 / 100));
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
	} else if (s == UI_FORGET_PHONE) {
		log_printf("bt: %s\n", bt_store_clear() ? "could not forget the phone" : "phone forgotten");
	} else if (s == SETTING_BLUETOOTH) {
		bool on = !setting_get(SETTING_BLUETOOTH);
		setting_set(SETTING_BLUETOOTH, on);
		if (on) {
			bluetooth_start();
		} else {
			bt_host_stop();
			bt_stop();
		}
	} else if (s >= 0 && s < SETTINGS) {
		setting_set((enum setting)s, !setting_get((enum setting)s));
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
	boot_init();
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
	bool key_was = false, awake = true, palm = false;
	int32_t passkey = -1;
	uint32_t notify_seen = 0, bt_start_t = time_ms() + BT_START_MS;
	uint32_t shown_seq = 0, newest_seq = 0, media_seen = 0, weather_seen = 0, weather_day = 0;
	int32_t dismissed_id;
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
			}
		}
		pmic_charger_poll();
		bms_poll();
		if (have_hub) {
			hub_poll();
			if (hub_tilted() && !awake)
				wake_request = true;
		}
		if (have_haptics)
			haptics_poll();
		while (have_touch && touch_poll(&t)) {
			if (t.event == TOUCH_PALM) {
				log_printf("touch: palm\n");
				sleep_request |= awake;
				palm = true;
				continue;
			}
			if (!awake) {
				if (t.event == TOUCH_DOWN)
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
		while (have_ui && ui_notification_dismissed(&dismissed_id)) {
			notify_dismiss(dismissed_id);
			last_activity = now;
		}
		if (notify_serial() != notify_seen) {
			notify_seen = notify_serial();
			unsigned n = notify_count();
			const struct notification *top = n ? notify_get(n - 1) : NULL;
			if ((top ? top->seq : 0) != shown_seq && have_haptics)
				haptics_play(NULL, 0);
			if (have_ui) {
				ui_clear_notifications();
				for (unsigned i = n; i--;) {
					const struct notification *e = notify_get(i);
					ui_add_notification(e->id, e->app, strlen(e->app), e->summary, strlen(e->summary), e->body,
							    strlen(e->body), e->icon, strlen(e->icon));
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
		for (int s; have_ui && (s = ui_setting_toggled()) >= 0;) {
			setting_toggled(s);
			last_activity = now;
		}
		for (int app; have_ui && (app = ui_app_opened()) >= 0;) {
			ui_set_app(app);
			stack_open_app();
			last_activity = now;
		}
		for (int action; have_ui && (action = ui_media_action()) >= 0;) {
			media_command((unsigned)action);
			last_activity = now;
		}
		if (have_ui && weather_serial() != weather_seen) {
			weather_seen = weather_serial();
			show_weather();
		}
		if (console_sleep || console_wake) {
			sleep_request |= console_sleep && awake;
			wake_request |= console_wake && !awake;
			console_sleep = console_wake = false;
		}
		if (have_display && awake && (sleep_request || now - last_activity >= SLEEP_AFTER_MS)) {
			ambient = have_ui && !palm && setting_get(SETTING_AOD);
			if (!ambient)
				display_sleep();
			if (have_ui) {
				stack_home();
				ui_hide_notification();
			}
			if (have_gpu) {
				gpu_suspend();
				gpu_off = true;
			}
			if (ambient) {
				ui_set_ambient(true);
				ambient_draw();
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
				ambient = false;
			} else {
				display_wake();
			}
			awake = true;
			asleep_ms += now - asleep_t;
			last_activity = now = time_ms();
			last_clock = 0;
			log_printf("wake\n");
		}
		sleep_request = wake_request = palm = false;
		if (ambient && !awake && pmic_rtc_read() / 60 != ambient_minute) {
			cpu_clock_slow(false);
			ambient_draw();
			cpu_clock_slow(true);
		}
		if (have_display && awake) {
			uint32_t *buf = display_backbuffer();
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
				if (repainted) {
					ui_repaints++;
					ui_repaint_us += time_us() - u0;
				}
				ui_us += time_us() - u0;
				ui_frames++;
				stack_offset(&sx, &sy);
				flatmesh_view(1.1f, (float)sx / fb.width * 0.05f, (float)sy / fb.height * 0.05f, 1.0f - stack_dim());
			}
			if (have_gpu) {
				gpu_flatmesh(buf, now - t0, have_ui);
			} else {
				flatmesh_render(buf, now - t0);
				if (have_ui)
					ui_render(buf, fb.stride);
			}
			if (!have_ui) {
				draw_dot(buf);
				fbcon_draw(buf);
			}
			display_flip();
		}
		if (have_ui && awake && (!last_clock || now - last_clock >= 1000)) {
			last_clock = now;
			struct civil c;
			uint32_t rtc = pmic_rtc_read();
			civil_from_epoch(rtc, &c);
			ui_set_clock(c.hour, c.minute, c.second, c.weekday, c.day, c.month);
			if (rtc / 86400 != weather_day) {
				weather_day = rtc / 86400;
				show_weather();
			}
			ui_set_battery(bms_percent(), pmic_charging());
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
			bool cluster = !awake && !usb_connected() && bt_can_sleep() && (!have_hub || hub_can_sleep());
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
				uint32_t minute = (60 - pmic_rtc_read() % 60) * 1000;
				if (ambient && minute < next)
					next = minute;
			}
			bool long_sleep = !awake && next > 1000;
			if (long_sleep) {
				timer_set_period(next);
				timer_next(next);
				wdt_stop();
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
