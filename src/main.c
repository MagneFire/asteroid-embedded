/* Bring-up and the main loop. The display round: console on the firmware's
 * own buffers, then the FlatMesh background at the panel's pace with the
 * console as an overlay, then brightness and panel sleep/wake, then a
 * reboot so the bootloader is back in fastboot for the next image. A
 * build-time TEST hangs or faults on purpose to exercise the crash path. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "gitrev.h"
#include "config.h"

const char build_id[] = GITREV;

#ifndef REBOOT_AFTER_MS
#define REBOOT_AFTER_MS 14000
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

static void mesh_phase(uint32_t t0, uint32_t ms)
{
	uint32_t start = time_ms(), frames0 = display_frames(), waits0 = display_waits();
	uint32_t render_us = 0, last_report = start;
	fbcon_set_overlay(true);
	while (time_ms() - start < ms) {
		wdt_feed();
		uint32_t now = time_ms(), r0 = time_us();
		flatmesh_render(display_backbuffer(), now - t0);
		fbcon_draw(display_backbuffer());
		render_us += time_us() - r0;
		display_flip();
		if (now - last_report >= 1000) {
			uint32_t f = display_frames() - frames0;
			log_printf("mesh: %lu frames, render %lu us/frame, wait %lu us/frame, %lu timeouts\n",
				   f, f ? render_us / f : 0, f ? (display_waits() - waits0) / f : 0, display_timeouts());
			last_report = now;
		}
	}
	fbcon_set_overlay(false);
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

	uint32_t t0 = time_ms(), last = 0;
	bool mesh_done = false, panel_done = false;
	for (;;) {
		uint32_t now = time_ms();
		wdt_feed();
		if (now - last >= 1000) {
			last = now;
			log_printf("tick %lu\n", (now - t0) / 1000);
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
		if (have_display && !mesh_done && now - t0 >= 2000) {
			mesh_phase(t0, 6000);
			mesh_done = true;
		}
		if (have_display && mesh_done && !panel_done && now - t0 >= 8000) {
			for (int level = 255; level >= 0; level -= 51) {
				display_set_brightness((uint8_t)level);
				log_printf("panel: brightness %d\n", level);
				delay_ms(250);
				wdt_feed();
			}
			display_set_brightness(255);
			log_printf("panel: sleep\n");
			display_sleep();
			for (int i = 0; i < 6; i++) {
				delay_ms(250);
				wdt_feed();
			}
			display_wake();
			log_printf("panel: awake, %lu frames so far\n", display_frames());
			panel_done = true;
		}
		if (now - t0 >= REBOOT_AFTER_MS) {
			log_printf("rebooting, %lu ticks\n", timer_ticks());
			reboot();
		}
		if (timer_ticks())
			wfi();
	}
}
