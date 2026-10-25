/* Bring-up and the main loop. For now the firmware shows what it found,
 * then reboots itself so the bootloader is back in fastboot for the next
 * image; a build-time TEST hangs or faults on purpose to exercise the
 * crash path. */
#include <string.h>
#include "asteroid.h"
#include "gitrev.h"
#include "config.h"

const char build_id[] = GITREV;

#ifndef REBOOT_AFTER_MS
#define REBOOT_AFTER_MS 5000
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

void main(void)
{
	mmu_init();
	gic_init();
	timer_init();
	dload_clear();
	wdt_arm(3000, 4000, wdt_bark_handler);
	irq_enable();
	log_init();
	log_printf("asteroid-embedded %s up, timer %lu Hz\n", build_id, timer_hz());
	spmi_init();
	pmic_init();
	log_printf("spmi arbiter %08lx, %s\n", spmi_version(), pmic_pon_reason());
	if (display_init()) {
		fbcon_init();
		log_set_sink(sink_fbcon);
	}
	fbcon_printf("asteroid-embedded %s\n%s\n", build_id, pmic_pon_reason());
	const char *prev = log_previous_record();
	if (prev) {
		log_puts("previous boot ended with:\n");
		log_puts(prev);
		log_puts("\n");
	} else
		log_printf("no fault record from the previous boot\n");

	uint32_t t0 = time_ms(), last = 0;
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
		if (now - t0 >= REBOOT_AFTER_MS) {
			log_printf("rebooting, %lu ticks\n", timer_ticks());
			reboot();
		}
		if (timer_ticks())
			wfi();
	}
}
