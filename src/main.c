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
	bool have_ui = have_display && ui_init((void *)UI_HEAP_BASE, UI_HEAP_SIZE, layers, UI_LAYER_STRIDE / 4, fb.stride,
					       fb.width, fb.height);
	if (have_ui) {
		ui_set_board(board->name, strlen(board->name), board->heart_rate, board->panel.round, board->light, !board->panel.lcd,
			     hub_measures_activity(), board->heart_rate && hub_has_passive());
		ui_set_timepiece(hub_has_timepiece());
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
		wdt_feed();
		usb_pass();
		usb_poll();
		console_poll();
		bt_poll();
		screenshot_poll();
		weather_poll();
		sched_run(now);
		app_sensors(now);
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
		app_power_changed(power_update(&now));
		present_clock(now);
		if (have_display && power_awake())
			frame_draw(now);
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
		power_idle(awake ? 1000 : app_next_ms());
	}
}
