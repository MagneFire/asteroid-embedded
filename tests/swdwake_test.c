/* The SWD clock's wake and hold on the host: fake pad, fake DHCSR, and
 * the real scheduler. */
#include <stdio.h>
#include <string.h>
#include "asteroid.h"
#include "sched.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static uint32_t now_ms, dhcsr, logs;
static bool irq_on, latched;
static void (*irq_fn)(unsigned n);
static unsigned irq_pad = ~0u;

uint32_t time_us(void) { return now_ms * 1000; }
uint32_t readl(uint32_t a) { return a == 0xe000edf0u ? dhcsr : 0; }
void writel(uint32_t v, uint32_t a) {}
void dsb(void) {}
void dmb(void) {}
void log_printf(const char *fmt, ...) { logs++; }
void gpio_swdck_irq(void (*fn)(unsigned n)) { irq_fn = fn; irq_pad = PAD_SWDCK; }
void gpio_irq_enable(unsigned n, bool on) { if (n == PAD_SWDCK) irq_on = on; }
void gpio_irq_ack(unsigned n) { if (n == PAD_SWDCK) latched = false; }
bool gpio_irq_latched(unsigned n) { return n == PAD_SWDCK && latched; }

/* A clock edge: latched always, an interrupt only while enabled. */
static void clock_edge(void)
{
	latched = true;
	if (irq_on) {
		latched = false;
		irq_fn(PAD_SWDCK);
	}
}

static void run_to(uint32_t t)
{
	while (now_ms < t) {
		now_ms++;
		sched_run(now_ms);
	}
}

int main(void)
{
	swdwake_init();
	CHECK(irq_pad == PAD_SWDCK && irq_on);
	CHECK(!swd_wanted());

	/* One edge holds light sleep for the hold, then deep sleep again. */
	clock_edge();
	CHECK(!irq_on && swd_wanted());
	sched_run(now_ms);
	CHECK(logs == 1);
	run_to(499);
	CHECK(swd_wanted() && !irq_on);
	run_to(500);
	CHECK(!swd_wanted() && irq_on);

	/* Traffic during a hold extends it, without interrupts. */
	clock_edge();
	sched_run(now_ms);
	uint32_t start = now_ms;
	for (unsigned i = 0; i < 5; i++) {
		run_to(now_ms + 300);
		clock_edge();
		CHECK(!irq_on);
	}
	/* The last edge came at 1500, right after a check: one more hold. */
	run_to(start + 2499);
	CHECK(swd_wanted());
	run_to(start + 2500);
	CHECK(!swd_wanted() && irq_on);
	CHECK(logs == 2);

	/* Debug on keeps it light with no hold. */
	dhcsr = 1;
	CHECK(swd_wanted());
	dhcsr = 0x00030000u;
	CHECK(!swd_wanted());

	/* Log lines for wakes 1, 2, 4, 8 only. */
	for (unsigned i = 3; i <= 9; i++) {
		clock_edge();
		run_to(now_ms + 1000);
	}
	CHECK(logs == 4);

	if (failures)
		printf("%d failures\n", failures);
	else
		printf("swdwake_test: ok\n");
	return failures != 0;
}
