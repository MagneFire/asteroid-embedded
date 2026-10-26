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
#include "gpu/shaders.h"
#include "ui/ui.h"
#include "gitrev.h"
#include "config.h"

const char build_id[] = GITREV;

#define SLEEP_AFTER_MS 10000

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
static uint32_t *const layer = (uint32_t *)(uintptr_t)UI_LAYER_BASE;

static void gpu_flatmesh(uint32_t *buf, uint32_t t_ms, bool with_layer)
{
	static float *vbo;
	if (!vbo)
		vbo = gpu_alloc(FLATMESH_GPU_VERTICES * 32, 64);
	uint32_t t0 = time_us();
	unsigned n = flatmesh_vertices_gpu(vbo, t_ms);
	struct gpu_target t = { (uint32_t)(uintptr_t)buf, fb.width, fb.height, fb.stride * 4 };
	dcache_invalidate_range(buf, fb.stride * fb.height * 4);
	uint32_t t1 = time_us();
	gpu_draw_begin(&t);
	gpu_draw_program(&shader_c_vs, &shader_c_fs);
	gpu_draw_triangles((uint32_t)(uintptr_t)vbo, 32, 2, (const uint8_t[]){4, 4}, n);
	if (with_layer)
		gpu_draw_layer((uint32_t)(uintptr_t)layer, fb.width, fb.height, fb.stride * 4);
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
	if (rpm_init())
		rpm_bus_votes();
	log_printf("spmi arbiter %08lx, %s\n", spmi_version(), pmic_pon_reason());
	bool have_display = display_init();
	if (have_display) {
		fbcon_init();
		log_set_sink(sink_fbcon);
	}
	fbcon_printf("asteroid-embedded %s\n", build_id);
	const char *prev = log_previous_record();
	if (prev) {
		log_puts("previous boot ended with:\n");
		log_puts(prev);
		log_puts("\n");
	} else
		log_printf("no fault record from the previous boot\n");

	pmic_charger_configure();
	spm_init();
	have_gpu = gpu_init() && gpu_selftest();
	bool have_ui = have_display && ui_init((void *)UI_HEAP_BASE, UI_HEAP_SIZE, layer, fb.stride, fb.width, fb.height);
	if (have_ui)
		dcache_clean_range(layer, fb.stride * fb.height * 4);
	log_printf("ui: %s\n", have_ui ? "ready" : "not started");
	tlmm_irq_init();
	bool have_touch = touch_init();
	console_init();
	usb_init("asteroid-sturgeon");
	if (have_display)
		display_set_idle_hook(usb_poll);
	log_printf("rtc %lu, battery %lu mV, %s\n", pmic_rtc_read(), pmic_battery_mv(), pmic_charger_status());

	uint32_t t0 = time_ms(), last = 0, last_clock = 0, frames0 = display_frames();
	uint32_t ui_frames = 0, ui_us = 0, ui_repaints = 0, ui_repaint_us = 0;
	bool sleep_request = false, wake_request = false, gpu_off = false;
	bool key_was = false, awake = true;
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
		if (!awake)
			wakeups++;
		while (have_touch && touch_poll(&t)) {
			if (!awake) {
				if (t.event == TOUCH_DOWN)
					wake_request = true;
				continue;
			}
			last_activity = now;
			if (have_ui)
				ui_touch(t.event == TOUCH_DOWN ? UI_TOUCH_DOWN : t.event == TOUCH_UP ? UI_TOUCH_UP : UI_TOUCH_MOVE, t.x, t.y);
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
		if (console_sleep || console_wake) {
			sleep_request |= console_sleep && awake;
			wake_request |= console_wake && !awake;
			console_sleep = console_wake = false;
		}
		if (have_display && awake && (sleep_request || now - last_activity >= SLEEP_AFTER_MS)) {
			display_sleep();
			if (have_gpu) {
				gpu_suspend();
				gpu_off = true;
			}
			timer_set_period(100);
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
			display_wake();
			awake = true;
			asleep_ms += now - asleep_t;
			last_activity = now = time_ms();
			last_clock = 0;
			log_printf("wake\n");
		}
		sleep_request = wake_request = false;
		if (have_display && awake) {
			uint32_t *buf = display_backbuffer();
			if (have_ui) {
				uint32_t u0 = time_us(), x, y, w, h;
				if (ui_update(&x, &y, &w, &h)) {
					if (have_gpu)
						for (uint32_t r = y; r < y + h; r++)
							dcache_clean_range(layer + r * fb.stride + x, w * 4);
					ui_repaints++;
					ui_repaint_us += time_us() - u0;
				}
				ui_us += time_us() - u0;
				ui_frames++;
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
			civil_from_epoch(pmic_rtc_read(), &c);
			ui_set_clock(c.hour, c.minute, c.second, c.weekday, c.day, c.month);
			ui_set_battery(pmic_battery_percent(pmic_battery_mv()), pmic_charging());
		}
		if (now - last >= 1000) {
			last = now;
			uint32_t rx, tx, att, terr;
			usb_stats(&rx, &tx);
			touch_stats(&att, &terr);
			log_printf("tick %lu, %lu fps, rtc %lu, battery %lu mV, %s, usb %lu/%lu, touch %lu/%lu\n", (now - t0) / 1000,
				   display_frames() - frames0, pmic_rtc_read(), pmic_battery_mv(), pmic_charger_status(), rx, tx, att, terr);
			frames0 = display_frames();
			uint32_t slept = asleep_ms + (awake ? 0 : now - asleep_t);
			uint32_t pc, pcb;
			spm_stats(&pc, &pcb);
			log_printf("power: %s, asleep %lu%% of the last second, %lu wakeups, %lu/%lu collapses, battery %ld mA\n",
				   awake ? "awake" : "asleep", slept > 1000 ? 100 : slept / 10, wakeups, pc, pcb,
				   pmic_battery_ua() / 1000);
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
		if ((!have_display || !awake) && timer_ticks())
			spm_cpu_idle_collapse();
	}
}
