/* Atmel maXTouch on smelt. The chip keeps its configuration in its own
 * memory, so nothing is written to it: the object table says where the
 * messages are, and the line going low says one is waiting. Each one is
 * read in interrupt context, the first finger's touches queued, and the
 * line unmasked again, so a line still low starts the next read at once.
 * After a failed read the line stays masked until mxt_poll retries it.
 * The controller's range comes from its T9 object and is scaled to the
 * panel. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "input/mxt.h"
#include "input/touch.h"

#define BUS		(board->touch_pins.i2c.bus)
#define ADDR		(board->touch_pins.i2c.addr)
#define GPIO_IRQ	(board->touch_pins.irq)
#define GPIO_RESET	(board->touch_pins.reset)
#define BUS_HZ		384000

#define INFO_ADDR	0
#define INFO_SIZE	7
#define TABLE_ADDR	7
#define TABLE_CHUNK	24
#define T9_RANGE	18
#define MSG_MAX		16
#define BOOT_WAIT_MS	500
#define RETRY_MIN_MS	50
#define RETRY_MAX_MS	2000

#define QUEUE_LEN	16
static struct touch queue[QUEUE_LEN];
static volatile unsigned q_head, q_tail;
static struct mxt_map map;
static uint8_t msg[MSG_MAX], msg_ptr[2];
static unsigned max_x = 1023, max_y = 1023;
static bool wide_x, wide_y, was_down;
static int last_x, last_y;
static uint32_t attentions, errors;
static volatile bool configuring, retry;
static uint8_t awake[MXT_POWER_WRITES];
static bool kept[MXT_POWER_WRITES];
static uint32_t retry_t, retry_ms;

static int rd(uint16_t addr, uint8_t *buf, unsigned n)
{
	uint8_t a[2] = { (uint8_t)addr, (uint8_t)(addr >> 8) };
	return i2c_xfer(BUS, ADDR, a, 2, buf, n);
}

static void push(const struct touch *t)
{
	unsigned next = (q_head + 1) % QUEUE_LEN;
	if (next == q_tail)
		return;
	queue[q_head] = *t;
	barrier();
	q_head = next;
}

static void rearm(void)
{
	if (configuring)
		return;
	gpio_irq_ack(GPIO_IRQ);
	gpio_irq_enable(GPIO_IRQ, true);
}

static void read_failed(void)
{
	errors++;
	retry_ms = !retry_ms ? RETRY_MIN_MS : retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : retry_ms * 2;
	retry_t = time_ms();
	retry = true;
}

static void message_done(int err, void *ctx)
{
	struct mxt_touch m;
	if (err) {
		read_failed();
		return;
	}
	retry_ms = 0;
	if (msg[0] != 0xff && mxt_palm(&map, msg)) {
		struct touch t = { TOUCH_PALM, last_x, last_y, false, time_ms() };
		was_down = false;
		push(&t);
	} else if (msg[0] != 0xff && mxt_decode(&map, msg, wide_x, wide_y, &m) && !m.finger) {
		struct touch t = { m.down ? (was_down ? TOUCH_MOVE : TOUCH_DOWN) : TOUCH_UP, last_x, last_y, m.down, time_ms() };
		if (m.down || (m.tap && !was_down)) {
			t.x = last_x = (int)((uint32_t)m.x * fb.width / (max_x + 1));
			t.y = last_y = (int)((uint32_t)m.y * fb.panel_height / (max_y + 1));
		}
		if (m.tap && !was_down) {
			struct touch d = t;
			d.event = TOUCH_DOWN;
			d.down = true;
			push(&d);
			push(&t);
		} else if (m.down || was_down)
			push(&t);
		was_down = m.down;
	}
	rearm();
}

static void attention(unsigned pin)
{
	gpio_irq_enable(GPIO_IRQ, false);
	attentions++;
	if (i2c_start(BUS, ADDR, msg_ptr, 2, msg, map.msg_size, message_done, NULL))
		read_failed();
}

/* Power, then a reset pulse; the line going low says it is up. */
static bool power_on(void)
{
	if (board->touch_pins.ldo && regulator_on(board->touch_pins.ldo, board->touch_pins.ldo_uv))
		log_printf("touch: ldo %u on not acknowledged\n", board->touch_pins.ldo);
	gpio_set(GPIO_RESET, 0);
	gpio_config(GPIO_RESET, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 6);
	delay_ms(2);
	gpio_set(GPIO_RESET, 1);
	uint32_t t0 = time_ms();
	while (gpio_get(GPIO_IRQ)) {
		if (time_ms() - t0 > BOOT_WAIT_MS)
			return false;
		wdt_feed();
	}
	return true;
}

static bool mxt_init(void)
{
	gpio_config(GPIO_IRQ, 0, GPIO_INPUT, GPIO_PULL_UP, 8);
	bool up = power_on();
	uint8_t info[INFO_SIZE], range[4];
	static uint8_t table[MXT_MAX_OBJECTS * MXT_OBJECT_SIZE];
	int ret = i2c_init(BUS, board->touch_pins.i2c.sda, board->touch_pins.i2c.scl, board->touch_pins.i2c.func, BUS_HZ);
	if (!ret)
		ret = rd(INFO_ADDR, info, INFO_SIZE);
	if (ret) {
		log_printf("touch: no answer on i2c (%d), line %s\n", ret, up ? "low" : "stayed high");
		return false;
	}
	unsigned objects = info[6];
	if (objects > MXT_MAX_OBJECTS) {
		log_printf("touch: %u objects\n", objects);
		return false;
	}
	for (unsigned off = 0; off < objects * MXT_OBJECT_SIZE; off += TABLE_CHUNK) {
		unsigned n = objects * MXT_OBJECT_SIZE - off;
		if ((ret = rd((uint16_t)(TABLE_ADDR + off), table + off, n < TABLE_CHUNK ? n : TABLE_CHUNK)))
			return false;
	}
	if (!mxt_parse(table, objects, &map) || map.msg_size > MSG_MAX) {
		log_printf("touch: no touch object among %u\n", objects);
		return false;
	}
	/* Before the 21st byte of T9 there is no range and 10 bits. */
	if (map.t9_size > T9_RANGE + 3 && !rd((uint16_t)(map.t9_addr + T9_RANGE), range, 4)) {
		wide_x = mxt_wide(range[1]);
		wide_y = mxt_wide(range[3]);
		max_x = (unsigned)(range[0] | range[1] << 8);
		max_y = (unsigned)(range[2] | range[3] << 8);
	}
	msg_ptr[0] = (uint8_t)map.msg_addr;
	msg_ptr[1] = (uint8_t)(map.msg_addr >> 8);
	log_printf("touch: mxt family %02x variant %02x version %02x build %02x, %u objects, range %u x %u, ids %u+%u\n", info[0],
		   info[1], info[2], info[3], objects, max_x, max_y, map.t9_first_id, map.t9_ids);
	if (board->touch_pins.own_settings) {
		for (unsigned i = 0; i < MXT_POWER_WRITES; i++) {
			uint16_t addr;
			uint8_t val;
			kept[i] = mxt_power_write(&map, i, false, &addr, &val) && !rd(addr, &awake[i], 1);
		}
		log_printf("touch: awake settings %02x %02x %02x %02x, threshold %u\n", awake[0], awake[1], awake[2], awake[3], awake[4]);
	}
	gpio_irq_config(GPIO_IRQ, GPIO_IRQ_LEVEL_LOW, attention);
	gpio_irq_enable(GPIO_IRQ, true);
	return true;
}

/* The vendor's low power settings while the screen sleeps. A message in
 * flight finishes first. */
static void mxt_sleep(bool on)
{
	if (!map.t9_addr)
		return;
	configuring = true;
	gpio_irq_enable(GPIO_IRQ, false);
	for (uint32_t t0 = time_ms(); i2c_busy(BUS) && time_ms() - t0 < 50;)
		i2c_poll(BUS);
	for (unsigned i = 0; i < MXT_POWER_WRITES; i++) {
		uint16_t addr;
		uint8_t val;
		if (board->touch_pins.own_settings && !kept[i])
			continue;
		if (mxt_power_write(&map, i, on, &addr, &val)) {
			if (!on && kept[i])
				val = awake[i];
			uint8_t w[3] = { (uint8_t)addr, (uint8_t)(addr >> 8), val };
			if (i2c_xfer(BUS, ADDR, w, 3, NULL, 0))
				errors++;
		}
	}
	configuring = false;
	if (!retry)
		rearm();
}

static uint32_t mxt_next_ms(void)
{
	return retry ? ms_left(retry_t, retry_ms, time_ms()) : UINT32_MAX;
}

static bool mxt_poll(struct touch *t)
{
	i2c_poll(BUS);
	if (retry && time_ms() - retry_t >= retry_ms) {
		retry = false;
		rearm();
	}
	if (q_tail == q_head)
		return false;
	barrier();
	*t = queue[q_tail];
	barrier();
	q_tail = (q_tail + 1) % QUEUE_LEN;
	return true;
}

static void mxt_stats(uint32_t *att, uint32_t *err)
{
	*att = attentions;
	*err = errors;
}

const struct touch_driver touch_mxt = { .init = mxt_init, .sleep = mxt_sleep, .poll = mxt_poll, .stats = mxt_stats,
					.next_ms = mxt_next_ms };
