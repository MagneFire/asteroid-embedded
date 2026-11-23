/* Himax HX852xES at 0x48. The chip keeps its firmware and settings in
 * its own flash, so the driver resets it, checks its id and reads
 * reports. A report pulses the interrupt line low and is one read of 16
 * bytes with command 0x86 (see hx852x.c), done in interrupt context and
 * queued; the edge that comes while it is read stays latched.
 *
 * Screen off, it goes to its idle mode, which scans slowly and only
 * sends a "Wake" packet for a touch, or with tap to wake off to sleep;
 * the vendor's commands for each change are run one by one from the main
 * loop with their waits between them. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "input/hx852x.h"
#include "input/touch.h"

#define BUS		(board->touch_pins.i2c.bus)
#define ADDR		(board->touch_pins.i2c.addr)
#define GPIO_IRQ	(board->touch_pins.irq)
#define GPIO_RESET	(board->touch_pins.reset)
#define BUS_HZ		100000

#define CMD_SLEEP_IN	0x80
#define CMD_SLEEP_OUT	0x81
#define CMD_SENSE_OFF	0x82
#define CMD_SENSE_ON	0x83
#define CMD_EVENTS	0x86
#define CMD_IDLE	0x8f
#define CMD_ID		0xd1
#define CMD_FW_MAJ	0x33
#define CMD_FW_MIN	0x32
#define CMD_CONFIG	0x39

#define RESET_MS	20
/* A finger the chip stopped reporting is let go after this long. */
#define LIFT_MS		200
#define STALL_MS	50

enum mode { ACTIVE, IDLE, SLEEP };

/* One command and the wait after it. */
struct step {
	uint8_t n, b[2];
	uint8_t ms;
};

static const struct step active_to_idle[] = { { 2, { CMD_IDLE, 0x20 }, 0 } };
static const struct step active_to_sleep[] = { { 1, { CMD_SENSE_OFF }, 40 }, { 1, { CMD_SLEEP_IN }, 0 } };
static const struct step idle_to_active[] = {
	{ 1, { CMD_SENSE_OFF }, 40 }, { 1, { CMD_SLEEP_IN }, 0 }, { 2, { CMD_IDLE, 0x00 }, 50 },
	{ 1, { CMD_SENSE_ON }, 30 },  { 1, { CMD_SLEEP_OUT }, 0 },
};
static const struct step idle_to_sleep[] = {
	{ 1, { CMD_SENSE_OFF }, 40 }, { 1, { CMD_SLEEP_IN }, 0 }, { 2, { CMD_IDLE, 0x00 }, 0 },
};
static const struct step sleep_to_active[] = { { 1, { CMD_SENSE_ON }, 30 }, { 1, { CMD_SLEEP_OUT }, 0 } };
static const struct step sleep_to_idle[] = {
	{ 1, { CMD_SENSE_ON }, 30 }, { 1, { CMD_SLEEP_OUT }, 50 }, { 2, { CMD_IDLE, 0x20 }, 0 },
};

#define SEQ(s) { s, sizeof s / sizeof s[0] }
static const struct {
	const struct step *s;
	unsigned n;
} seqs[3][3] = {
	[ACTIVE] = { [IDLE] = SEQ(active_to_idle), [SLEEP] = SEQ(active_to_sleep) },
	[IDLE] = { [ACTIVE] = SEQ(idle_to_active), [SLEEP] = SEQ(idle_to_sleep) },
	[SLEEP] = { [ACTIVE] = SEQ(sleep_to_active), [IDLE] = SEQ(sleep_to_idle) },
};

#define QUEUE_LEN	16
static struct touch queue[QUEUE_LEN];
static volatile unsigned q_head, q_tail;
static volatile bool round_busy, was_down, palm_down, holding;
static volatile uint32_t round_t, report_t;
static const uint8_t events_cmd = CMD_EVENTS;
static uint8_t report[HX852X_REPORT_LEN];
static uint32_t attentions, errors, dropped, stalls;
static bool present;
static volatile enum mode mode;
static enum mode want;
/* The change under way: its steps, the next one and when it is due. */
static const struct step *seq;
static unsigned seq_n, seq_i;
static enum mode seq_to;
static uint32_t step_t, step_wait;
static int cur_id, last_x, last_y;

static int rd(uint8_t cmd, uint8_t *buf, unsigned n)
{
	return i2c_xfer(BUS, ADDR, &cmd, 1, buf, n);
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
	if (!holding && mode != SLEEP)
		gpio_irq_enable(GPIO_IRQ, true);
}

static int scale(unsigned v, unsigned size)
{
	int s = (int)(v * size / HX852X_RANGE);
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
	struct hx852x_report r;
	hx852x_parse(report, &r);
	if (r.bad) {
		dropped++;
		rearm();
		return;
	}
	report_t = time_ms();
	const struct hx852x_point *f = hx852x_follow(&r, was_down ? cur_id : -1);
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

/* In idle mode a touch is a tap where the finger last was. */
static void wake_done(int err, void *ctx)
{
	if (err) {
		errors++;
	} else if (hx852x_is_wake(report)) {
		struct touch t = { TOUCH_DOWN, last_x, last_y, true, 0 };
		push(&t);
		t.event = TOUCH_UP;
		t.down = false;
		push(&t);
	} else {
		dropped++;
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
	bool idle = mode == IDLE;
	if (i2c_start(BUS, ADDR, &events_cmd, 1, report, idle ? HX852X_WAKE_LEN : HX852X_REPORT_LEN,
		      idle ? wake_done : report_done, NULL)) {
		errors++;
		rearm();
	}
}

static void attention(unsigned pin)
{
	if (!round_busy && !holding)
		start_round();
}

/* An edge missed (a failed round with the line still low) would stop the
 * reports for good, so a line found low with no round for a while
 * starts one from here. */
static void stall_poll(void)
{
	if (!present || round_busy || holding || mode == SLEEP || gpio_get(GPIO_IRQ) || time_ms() - round_t < STALL_MS)
		return;
	uint32_t flags = irq_save();
	if (!round_busy) {
		start_round();
		stalls++;
	}
	irq_restore(flags);
}

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

/* The pin interrupt is held off for a change of mode, and a round in
 * flight finished first. */
static void begin_change(void)
{
	if (seq || mode == want)
		return;
	seq = seqs[mode][want].s;
	seq_n = seqs[mode][want].n;
	seq_to = want;
	seq_i = 0;
	step_wait = 0;
	step_t = time_ms();
	uint32_t flags = irq_save();
	holding = true;
	gpio_irq_enable(GPIO_IRQ, false);
	irq_restore(flags);
}

static void end_change(void)
{
	seq = NULL;
	uint32_t flags = irq_save();
	mode = seq_to;
	holding = false;
	wake_pin_touch(mode != SLEEP);
	if (mode != SLEEP && !round_busy) {
		round_t = time_ms();
		gpio_irq_enable(GPIO_IRQ, true);
	}
	irq_restore(flags);
	log_printf("touch: %s\n", mode == ACTIVE ? "active" : mode == IDLE ? "idle" : "asleep");
	begin_change();
}

static void change_poll(void)
{
	if (!seq || round_busy || time_ms() - step_t < step_wait)
		return;
	const struct step *s = &seq[seq_i];
	if (i2c_xfer(BUS, ADDR, s->b, s->n, NULL, 0))
		errors++;
	step_t = time_ms();
	step_wait = s->ms;
	if (++seq_i == seq_n)
		end_change();
}

static void hx852x_sleep(bool on, bool wakes)
{
	if (!present)
		return;
	if (on)
		release();
	want = !on ? ACTIVE : wakes ? IDLE : SLEEP;
	begin_change();
}

static uint32_t hx852x_next_ms(void)
{
	if (!present)
		return UINT32_MAX;
	uint32_t now = time_ms();
	if (seq)
		return round_busy ? 1 : ms_left(step_t, step_wait, now);
	return was_down || palm_down ? ms_left(report_t, LIFT_MS, now) : UINT32_MAX;
}

static struct {
	bool active, down;
	int x0, y0, x1, y1;
	uint32_t t0, ms, last;
} fake;

static void hx852x_inject(int x0, int y0, int x1, int y1, uint32_t ms)
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

static bool hx852x_poll(struct touch *t)
{
	i2c_poll(BUS);
	change_poll();
	stall_poll();
	if (present && mode == ACTIVE && (was_down || palm_down) && !round_busy && time_ms() - report_t >= LIFT_MS)
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

static void hx852x_stats(uint32_t *att, uint32_t *err)
{
	*att = attentions;
	*err = errors + dropped;
}

/* Reset held low while the supply comes up, then let go, with the
 * vendor's waits. */
static void power_on(void)
{
	gpio_set(GPIO_RESET, 0);
	gpio_config(GPIO_RESET, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	if (board->touch_pins.ldo && regulator_on(board->touch_pins.ldo, board->touch_pins.ldo_uv))
		log_printf("touch: ldo %u on not acknowledged\n", board->touch_pins.ldo);
	wait_ms(RESET_MS);
	gpio_set(GPIO_RESET, 1);
	wait_ms(RESET_MS);
}

static void command(uint8_t cmd, uint32_t ms)
{
	if (i2c_xfer(BUS, ADDR, &cmd, 1, NULL, 0))
		errors++;
	wait_ms(ms);
}

static bool hx852x_init(void)
{
	gpio_config(GPIO_IRQ, 0, GPIO_INPUT, GPIO_PULL_UP, 2);
	power_on();
	uint8_t id[3] = { 0 };
	int ret = i2c_init(BUS, board->touch_pins.i2c.sda, board->touch_pins.i2c.scl, board->touch_pins.i2c.func, BUS_HZ);
	if (!ret)
		ret = rd(CMD_ID, id, sizeof id);
	if (ret || id[0] != 0x05 || id[1] != 0x85 || id[2] < 0x25 || id[2] > 0x28) {
		log_printf("touch: no HX852xES (%d, id %02x %02x %02x), irq gpio %u\n", ret, id[0], id[1], id[2],
			   gpio_get(GPIO_IRQ));
		return false;
	}
	uint8_t maj = 0, min = 0, cfg = 0;
	rd(CMD_FW_MAJ, &maj, 1);
	rd(CMD_FW_MIN, &min, 1);
	rd(CMD_CONFIG, &cfg, 1);
	/* The vendor's start: sense on and out of sleep, back to sleep, and
	 * on again. */
	command(CMD_SENSE_ON, 30);
	command(CMD_SLEEP_OUT, 50);
	command(CMD_SENSE_OFF, 50);
	command(CMD_SLEEP_IN, 50);
	command(CMD_SENSE_ON, 30);
	command(CMD_SLEEP_OUT, 50);
	log_printf("touch: HX852%02xES firmware %02x.%02x, config %02x, irq gpio %u\n", id[2], maj, min, cfg,
		   gpio_get(GPIO_IRQ));

	gpio_irq_config(GPIO_IRQ, GPIO_IRQ_EDGE_FALLING, attention);
	round_t = report_t = time_ms();
	mode = want = ACTIVE;
	present = true;
	gpio_irq_enable(GPIO_IRQ, true);
	return true;
}

const struct touch_driver touch_hx852x = {
	.init = hx852x_init,
	.poll = hx852x_poll,
	.stats = hx852x_stats,
	.inject = hx852x_inject,
	.sleep = hx852x_sleep,
	.next_ms = hx852x_next_ms,
};
