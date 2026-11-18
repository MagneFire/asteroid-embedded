/* Bring-up and the main loop: each pass polls the drivers and services,
 * handles what they report, draws a frame if one is due and then waits. */
#include <stddef.h>
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "input/touch.h"
#include "console/console.h"
#include "gpu/a3xx.h"
#include "ui/ui.h"
#include "ui/stack.h"
#include "ui/rotation.h"
#include "bt/bt.h"
#include "sensors/hub.h"
#include "weather/weather.h"
#include "screenshot/screenshot.h"
#include "clockapps.h"
#include "game.h"
#include "power.h"
#include "frame.h"
#include "stats.h"
#include "app.h"
#include "sched.h"
#include "present.h"
#include "config.h"
#include "storage/emmc.h"

#ifndef REBOOT_AFTER_MS
#define REBOOT_AFTER_MS 0
#endif

/* For LAG_WINDOW_MS after a wake, where the time of the wake's own pass
 * and of the slowest pass after it went, logged once the window ends.
 * Sleeping with nothing to draw is not counted. */
#define LAG_WINDOW_MS 3000
#define LAG_SLOW_US 50000
enum { LAG_START, LAG_SCHED, LAG_SENSORS, LAG_EVENTS, LAG_UPDATE, LAG_CHANGED, LAG_CLOCK, LAG_FRAME, LAG_MARKS };
static uint32_t lag_t[LAG_MARKS], lag_wake[LAG_MARKS], lag_worst[LAG_MARKS], lag_woke, lag_slow, lag_ticks, lag_waited;
static bool lag_open;

static void lag_mark(unsigned i) { lag_t[i] = time_us(); }

static void lag_take(uint32_t *to)
{
	for (unsigned i = 1; i < LAG_MARKS; i++)
		to[i] = lag_t[i] - lag_t[i - 1];
	to[LAG_FRAME] -= lag_waited;
	to[0] = lag_t[LAG_FRAME] - lag_t[LAG_START] - lag_waited;
}

static void lag_say(const char *what, const uint32_t *p)
{
	log_printf("lag: %s %lu us (sched %lu, sensors %lu, events %lu, power %lu, woke %lu, clock %lu, frame %lu)\n", what,
		   p[0], p[LAG_SCHED], p[LAG_SENSORS], p[LAG_EVENTS], p[LAG_UPDATE], p[LAG_CHANGED], p[LAG_CLOCK],
		   p[LAG_FRAME]);
}

static const char *const lag_parts[LAG_MARKS] = { "", "sched", "sensors", "events", "power", "woke", "clock", "frame" };

static void lag_pass(uint32_t now, bool woke)
{
	uint32_t pass[LAG_MARKS];
	lag_take(pass);
	unsigned most = 1;
	for (unsigned i = 2; i < LAG_MARKS; i++)
		if (pass[i] > pass[most])
			most = i;
	app_pass(now, pass[0], lag_parts[most], woke ? now : lag_woke);
	if (woke) {
		lag_open = true;
		lag_woke = now;
		lag_slow = 0;
		lag_ticks = timer_ticks();
		memcpy(lag_wake, pass, sizeof pass);
		memset(lag_worst, 0, sizeof lag_worst);
		return;
	}
	if (!lag_open)
		return;
	lag_slow += pass[0] >= LAG_SLOW_US;
	if (pass[0] > lag_worst[0])
		memcpy(lag_worst, pass, sizeof pass);
	if (now - lag_woke < LAG_WINDOW_MS && power_awake())
		return;
	lag_open = false;
	lag_say("wake pass", lag_wake);
	lag_say("slowest after", lag_worst);
	log_printf("lag: %lu passes over %u ms, %lu ticks in %lu ms\n", lag_slow, LAG_SLOW_US / 1000, timer_ticks() - lag_ticks,
		   now - lag_woke);
}

extern void wdt_bark_handler(void);
static uint32_t *const layers = (uint32_t *)(uintptr_t)UI_LAYER_BASE;

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

void main(uint32_t machtype, const void *fdt)
{
	boot_stage(BOOT_STAGE("main"));
	soc_init();
	boot_stage(BOOT_STAGE("wdt "));
	wdt_arm(3000, 4000, wdt_bark_handler);
	irq_enable();
	boot_stage(BOOT_STAGE("log "));
	log_init();
	boot_stage(BOOT_STAGE("up  "));
	if (boot_previous_stage())
		log_printf("previous boot stopped at stage %s\n", boot_previous_stage());
	log_printf("asteroid-embedded %s up, timer %lu Hz\n", build_id, timer_hz());
	cpu_clock_log();
	board_detect(machtype, fdt);
	soc_board_init();
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
	if (log_ring_lost())
		log_printf("no log from the previous boot: cold power-on, or another OS ran\n");

	batt_init();
	light_init();
	cpu_idle_init();
	bool have_gpu = gpu_init() && gpu_selftest();
	bool have_ui = have_display && ui_init((void *)UI_HEAP_START, UI_HEAP_BASE + UI_HEAP_SIZE - UI_HEAP_START, layers, UI_LAYER_STRIDE / 4, fb.stride,
					       fb.width, fb.height);
	if (have_ui) {
		ui_set_board(board->name, strlen(board->name), board->heart_rate, board->panel.round, board->light, !board->panel.lcd,
			     hub_measures_activity(), board->heart_rate && hub_has_passive());
		ui_set_timepiece(hub_has_timepiece());
		ui_set_tilt_light(hub_has_screen());
		stack_init(fb.width, fb.height, layers, UI_LAYER_STRIDE / 4);
		if (board->panel.edge_px)
			stack_set_edge(board->panel.edge_px);
		stack_set_round(board->panel.round);
		/* Only a square picture can turn; off otherwise. */
		rotation_init(fb.width == fb.height ? (int)fb.width : 0);
		stack_set_chin((int)(fb.height - fb.panel_height));
		ui_set_chin(fb.height - fb.panel_height);
		for (unsigned p = 0; p < UI_PANELS; p++)
			dcache_clean_range(layers + p * (UI_LAYER_STRIDE / 4), fb.stride * fb.height * 4);
	}
	log_printf("ui: %s\n", have_ui ? "ready" : "not started");
	if (have_display && !have_ui)
		log_set_sink(sink_fbcon);
	gpio_irq_init();
	power_key_irq_init();
	batt_cable_irq_init();
	bool have_touch = touch_init();
	bool have_haptics = haptics_init();
	bool have_hub = hub_init();
	power_init(have_display, have_ui, have_hub);
	console_init();
	static char serialno[24];
	snprintf(serialno, sizeof serialno, "asteroid-%s", board->name);
	usb_init(serialno);
	if (have_display)
		display_set_idle_hook(usb_poll);
	log_printf("rtc %lu, battery %lu mV, %s\n", rtc_read(), batt_mv(), batt_status());

	uint32_t t0 = time_ms();
	frame_init(have_ui, have_gpu, t0);
	stats_init(t0);
	clockapps_init(have_ui, have_haptics);
	game_init();
	present_init(have_ui, have_haptics, have_hub);
	app_init(&(struct app_caps){ have_display, have_ui, have_touch, have_haptics, have_hub, have_hub && board->heart_rate });
	if (have_display)
		fbcon_set_overlay(true);
	for (;;) {
		uint32_t now = time_ms();
		lag_mark(LAG_START);
		wdt_feed();
		usb_pass();
		usb_poll();
		console_poll();
		bt_poll();
		screenshot_poll();
		weather_poll();
		sched_run(now);
		lag_mark(LAG_SCHED);
		app_sensors(now);
		lag_mark(LAG_SENSORS);
		app_flashlight(now);
		if (have_haptics)
			haptics_poll();
		app_touch(now);
		bool key_irq = power_key_irq();
		app_cable(now);
		events_poll(now);
		app_key(now, key_irq);
		present_events(now);
		app_tick(now);
		app_ui_events(now);
		app_after_events(now);
		app_console_settings();
		lag_mark(LAG_EVENTS);
		enum power_change change = power_update(&now);
		lag_mark(LAG_UPDATE);
		app_power_changed(change);
		lag_mark(LAG_CHANGED);
		present_clock(now);
		lag_mark(LAG_CLOCK);
		lag_waited = 0;
		if (have_display && power_awake()) {
			frame_draw(now);
			lag_waited = frame_waited_us();
		}
		lag_mark(LAG_FRAME);
		lag_pass(now, change == POWER_WOKE);
		bool awake = power_awake();
		stats_poll(now, awake);
#if REBOOT_AFTER_MS > 0
		if (now - t0 >= REBOOT_AFTER_MS) {
			log_printf("rebooting, %lu ticks\n", timer_ticks());
			reboot();
		}
#endif
		if (!awake)
			emmc_idle();
		uint32_t rest = awake && have_display ? frame_rest_ms() : 0;
		if (rest) {
			uint32_t next = app_awake_next_ms();
			power_rest(rest < next ? rest : next);
		} else {
			const char *why = "awake";
			power_idle(awake ? 1000 : app_next_ms(&why), why);
		}
	}
}
