/* FocalTech FT3267 at 0x38. A touch pulses the interrupt line low and
 * the whole report is one read of 21 bytes from register 0 (see fts.c).
 * The chip keeps its own firmware and settings, so nothing is written
 * to it: the driver only resets it, checks its id and reads reports.
 * Each one is read in interrupt context and queued; the edge that comes
 * while it is read stays latched. The chip needs its two supplies and a
 * switch line held high to answer. It keeps scanning while the screen
 * is off, so a touch wakes the watch like any other. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "input/fts.h"
#include "input/touch.h"

#define BUS		(board->touch_pins.i2c.bus)
#define ADDR		(board->touch_pins.i2c.addr)
#define GPIO_IRQ	(board->touch_pins.irq)
#define GPIO_RESET	(board->touch_pins.reset)
#define GPIO_POWER	(board->touch_pins.power)
#define BUS_HZ		384000

#define REG_REPORT	0x00
#define REG_CHIP_ID	0xa3
#define REG_FIRMWARE	0xa6
#define REG_VENDOR	0xa8
#define CHIP_ID		0x33

#define SUPPLY_MS	170
#define RESET_LOW_MS	6
#define BOOT_MS		200
#define ID_POLL_MS	20
#define ID_WAIT_MS	300
/* A finger the chip stopped reporting is let go after this long. */
#define LIFT_MS		200
#define STALL_MS	50

#define QUEUE_LEN	16
static struct touch queue[QUEUE_LEN];
static volatile unsigned q_head, q_tail;
static volatile bool round_busy, was_down, palm_down;
static volatile uint32_t round_t, report_t;
static const uint8_t report_reg = REG_REPORT;
static uint8_t report[FTS_REPORT_LEN];
static uint32_t attentions, errors, stalls;
static bool present;
static int cur_id, last_x, last_y;

static int rd(uint8_t reg, uint8_t *buf, unsigned n)
{
	return i2c_xfer(BUS, ADDR, &reg, 1, buf, n);
}

static void wait_ms(uint32_t ms)
{
	for (; ms; ms -= ms < 10 ? ms : 10) {
		delay_ms(ms < 10 ? ms : 10);
		wdt_feed();
	}
}

static void push(const struct touch *t)
{
	unsigned next = (q_head + 1) % QUEUE_LEN;
	if (next == q_tail)
		return;
	queue[q_head] = *t;
	queue[q_head].ms = time_ms();
	barrier();
	q_head = next;
}

static void rearm(void)
{
	round_busy = false;
	round_t = time_ms();
	gpio_irq_enable(GPIO_IRQ, true);
}

static int scale(unsigned v, unsigned size)
{
	int s = (int)(v * size / FTS_RANGE);
	return s >= (int)size ? (int)size - 1 : s;
}

/* One finger is followed, from its first report until it lifts. A palm
 * lifts it and suppresses fingers until a report without one. */
static void report_done(int err, void *ctx)
{
	if (err) {
		errors++;
		rearm();
		return;
	}
	struct fts_report r;
	fts_parse(report, &r);
	report_t = time_ms();
	const struct fts_point *f = fts_follow(&r, was_down ? cur_id : -1);
	struct touch t = { TOUCH_NONE, last_x, last_y, false, 0 };
	if (r.palm && !palm_down) {
		if (was_down) {
			t.event = TOUCH_UP;
			push(&t);
			was_down = false;
		}
		t.event = TOUCH_PALM;
		push(&t);
		palm_down = true;
	} else if (palm_down) {
		if (!r.palm && !f)
			palm_down = false;
	} else if (f) {
		t.x = last_x = scale(f->x, fb.width);
		t.y = last_y = scale(f->y, fb.height);
		t.event = was_down ? TOUCH_MOVE : TOUCH_DOWN;
		t.down = true;
		cur_id = f->id;
		was_down = true;
		push(&t);
	} else if (was_down) {
		t.event = TOUCH_UP;
		was_down = false;
		push(&t);
	}
	rearm();
}

/* Called with interrupts masked. */
static void start_round(void)
{
	gpio_irq_enable(GPIO_IRQ, false);
	gpio_irq_ack(GPIO_IRQ);
	round_busy = true;
	round_t = time_ms();
	attentions++;
	if (i2c_start(BUS, ADDR, &report_reg, 1, report, sizeof report, report_done, NULL)) {
		errors++;
		rearm();
	}
}

static void attention(unsigned pin)
{
	if (!round_busy)
		start_round();
}

/* An edge missed (a failed round with the line still low) would stop the
 * reports for good, so a line found low with no round for a while
 * starts one from here. */
static void stall_poll(void)
{
	if (!present || round_busy || gpio_get(GPIO_IRQ) || time_ms() - round_t < STALL_MS)
		return;
	uint32_t flags = irq_save();
	if (!round_busy) {
		start_round();
		stalls++;
	}
	irq_restore(flags);
}

/* Fingers are let go when the chip stops reporting them, or when the screen
 * goes off, so none is held down across the sleep. */
static void release(void)
{
	uint32_t flags = irq_save();
	if (was_down) {
		struct touch t = { TOUCH_UP, last_x, last_y, false, 0 };
		push(&t);
		was_down = false;
	}
	palm_down = false;
	irq_restore(flags);
}

static void fts_sleep(bool on)
{
	if (present && on)
		release();
}

static uint32_t fts_next_ms(void)
{
	return present && (was_down || palm_down) ? ms_left(report_t, LIFT_MS, time_ms()) : UINT32_MAX;
}

static struct {
	bool active, down;
	int x0, y0, x1, y1;
	uint32_t t0, ms, last;
} fake;

static void fts_inject(int x0, int y0, int x1, int y1, uint32_t ms)
{
	fake.x0 = x0;
	fake.y0 = y0;
	fake.x1 = x1;
	fake.y1 = y1;
	fake.ms = ms ? ms : 1;
	fake.down = false;
	fake.active = true;
}

static void fake_poll(void)
{
	uint32_t now = time_ms();
	struct touch t = { TOUCH_DOWN, fake.x0, fake.y0, true, 0 };
	if (!fake.down) {
		fake.down = true;
		fake.t0 = fake.last = now;
	} else if (now - fake.t0 >= fake.ms) {
		t = (struct touch){ TOUCH_UP, fake.x1, fake.y1, false, 0 };
		fake.active = false;
	} else if (now - fake.last >= 16) {
		int32_t f = (int32_t)((now - fake.t0) * 1024 / fake.ms);
		t = (struct touch){ TOUCH_MOVE, fake.x0 + (fake.x1 - fake.x0) * f / 1024, fake.y0 + (fake.y1 - fake.y0) * f / 1024,
				    true, 0 };
		fake.last = now;
	} else {
		return;
	}
	uint32_t flags = irq_save();
	push(&t);
	irq_restore(flags);
}

static bool fts_poll(struct touch *t)
{
	i2c_poll(BUS);
	stall_poll();
	if (present && (was_down || palm_down) && !round_busy && time_ms() - report_t >= LIFT_MS)
		release();
	if (fake.active)
		fake_poll();
	if (q_tail == q_head)
		return false;
	barrier();
	*t = queue[q_tail];
	barrier();
	q_tail = (q_tail + 1) % QUEUE_LEN;
	return true;
}

static void fts_stats(uint32_t *att, uint32_t *err)
{
	*att = attentions;
	*err = errors;
}

/* Both supplies and the switch line, then a reset pulse (low for 6 ms);
 * the chip answers about 200 ms later. */
static void power_on(void)
{
	if (board->touch_pins.ldo && regulator_on(board->touch_pins.ldo, board->touch_pins.ldo_uv))
		log_printf("touch: ldo %u on not acknowledged\n", board->touch_pins.ldo);
	if (board->touch_pins.ldo_io && regulator_on(board->touch_pins.ldo_io, board->touch_pins.ldo_io_uv))
		log_printf("touch: ldo %u on not acknowledged\n", board->touch_pins.ldo_io);
	if (GPIO_POWER) {
		gpio_set(GPIO_POWER, 1);
		gpio_config(GPIO_POWER, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	}
	wait_ms(SUPPLY_MS);
	gpio_set(GPIO_RESET, 1);
	gpio_config(GPIO_RESET, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	delay_ms(2);
	gpio_set(GPIO_RESET, 0);
	delay_ms(RESET_LOW_MS);
	gpio_set(GPIO_RESET, 1);
	wait_ms(BOOT_MS);
}

/* The chip answers its id once it has booted. */
static int wait_chip(uint8_t *id)
{
	for (uint32_t waited = 0;; waited += ID_POLL_MS) {
		int ret = rd(REG_CHIP_ID, id, 1);
		if (!ret && *id == CHIP_ID)
			return 0;
		if (waited >= ID_WAIT_MS)
			return ret ? ret : -1;
		wait_ms(ID_POLL_MS);
	}
}

static bool fts_init(void)
{
	gpio_config(GPIO_IRQ, 0, GPIO_INPUT, GPIO_PULL_UP, 2);
	power_on();
	uint8_t id = 0;
	int ret = i2c_init(BUS, board->touch_pins.i2c.sda, board->touch_pins.i2c.scl, board->touch_pins.i2c.func, BUS_HZ);
	if (!ret)
		ret = wait_chip(&id);
	if (ret) {
		log_printf("touch: no FT3267 (%d, id %02x), irq gpio %u\n", ret, id, gpio_get(GPIO_IRQ));
		return false;
	}
	uint8_t fw = 0, vendor = 0;
	rd(REG_FIRMWARE, &fw, 1);
	rd(REG_VENDOR, &vendor, 1);
	log_printf("touch: FT3267 firmware %02x, vendor %02x, irq gpio %u\n", fw, vendor, gpio_get(GPIO_IRQ));

	gpio_irq_config(GPIO_IRQ, GPIO_IRQ_EDGE_FALLING, attention);
	round_t = report_t = time_ms();
	present = true;
	gpio_irq_enable(GPIO_IRQ, true);
	return true;
}

const struct touch_driver touch_fts = {
	.init = fts_init,
	.poll = fts_poll,
	.stats = fts_stats,
	.inject = fts_inject,
	.sleep = fts_sleep,
	.next_ms = fts_next_ms,
};
