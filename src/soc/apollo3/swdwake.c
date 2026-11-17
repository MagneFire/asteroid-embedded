/* In deep sleep the debug port answers WAIT to every memory access, and
 * the application processor's driver resets the MCU after too many. The
 * SWD clock's first edge wakes the core instead; one interrupt starts a
 * hold, and the core sleeps only lightly until the clock has been quiet
 * for a whole hold, and while debug is on. */
#include "asteroid.h"
#include "sched.h"

#define HOLD_MS		500
#define DHCSR		0xe000edf0u
#define C_DEBUGEN	BIT(0)

static volatile bool held;
static bool timing;
static uint32_t wakes;

static bool hold_run(uint32_t now);
static struct sched_task task = { .name = "swd wake", .run = hold_run };

static void edge(unsigned n)
{
	gpio_irq_enable(PAD_SWDCK, false);
	held = true;
	sched_post(&task);
}

/* Edges during a hold are only latched; any of them starts another. An
 * edge between the check and the ack is lost, but the AP's next retry
 * brings one. */
static bool hold_run(uint32_t now)
{
	if (!timing) {
		if (!held)
			return false;
		timing = true;
		wakes++;
		/* Sparse, for a clock line left floating. */
		if (!(wakes & (wakes - 1)))
			log_printf("swd: woken by the debug clock, %lu times\n", wakes);
	} else if (gpio_irq_latched(PAD_SWDCK)) {
		gpio_irq_ack(PAD_SWDCK);
	} else {
		timing = false;
		held = false;
		gpio_irq_ack(PAD_SWDCK);
		gpio_irq_enable(PAD_SWDCK, true);
		return false;
	}
	sched_after(&task, now, HOLD_MS);
	return false;
}

void swdwake_init(void)
{
	if (!sched_add(&task))
		return;
	gpio_swdck_irq(edge);
	gpio_irq_enable(PAD_SWDCK, true);
}

bool swd_wanted(void)
{
	return held || (readl(DHCSR) & C_DEBUGEN);
}
