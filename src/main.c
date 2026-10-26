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
#include "gitrev.h"
#include "config.h"

const char build_id[] = GITREV;

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

void main(void)
{
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
	rpm_init();
	if (gpu_init())
		gpu_selftest();
	tlmm_irq_init();
	bool have_touch = touch_init();
	console_init();
	usb_init("asteroid-sturgeon");
	if (have_display)
		display_set_idle_hook(usb_poll);
	log_printf("rtc %lu, battery %lu mV, %s\n", pmic_rtc_read(), pmic_battery_mv(), pmic_charger_status());

	uint32_t t0 = time_ms(), last = 0, frames0 = display_frames();
	bool key_was = false;
	struct touch t;
	int down_x = 0, down_y = 0;
	uint32_t down_t = 0;
	if (have_display)
		fbcon_set_overlay(true);
	for (;;) {
		uint32_t now = time_ms();
		wdt_feed();
		usb_poll();
		while (have_touch && touch_poll(&t)) {
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
			log_printf("power key %s\n", key ? "pressed" : "released");
			key_was = key;
			if (!key) {
				log_printf("rebooting on power key\n");
				reboot();
			}
		}
		if (have_display) {
			uint32_t *buf = display_backbuffer();
			flatmesh_render(buf, now - t0);
			draw_dot(buf);
			fbcon_draw(buf);
			display_flip();
		}
		if (now - last >= 1000) {
			last = now;
			uint32_t rx, tx, att, terr;
			usb_stats(&rx, &tx);
			touch_stats(&att, &terr);
			log_printf("tick %lu, %lu fps, rtc %lu, battery %lu mV, %s, usb %lu/%lu, touch %lu/%lu\n", (now - t0) / 1000,
				   display_frames() - frames0, pmic_rtc_read(), pmic_battery_mv(), pmic_charger_status(), rx, tx, att, terr);
			frames0 = display_frames();
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
		if (REBOOT_AFTER_MS > 0 && now - t0 >= (uint32_t)REBOOT_AFTER_MS) {
			log_printf("rebooting, %lu ticks\n", timer_ticks());
			reboot();
		}
		if (!have_display && timer_ticks())
			wfi();
	}
}
