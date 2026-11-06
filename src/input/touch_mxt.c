/* Atmel maXTouch on smelt. The chip keeps its configuration in its own
 * memory, so nothing is written to it: the object table says where the
 * messages are, and the line going low says one is waiting. Each one is
 * read in interrupt context, the first finger's touches queued, and the
 * line unmasked again, so a line still low starts the next read at once.
 * The controller's range comes from its T9 object and is scaled to the
 * panel. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "input/mxt.h"
#include "input/touch.h"

#define QUP		(board->touch_pins.i2c.qup)
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

#define QUEUE_LEN	16
static struct touch queue[QUEUE_LEN];
static volatile unsigned q_head, q_tail;
static struct mxt_map map;
static uint8_t msg[MSG_MAX], msg_ptr[2];
static unsigned max_x = 1023, max_y = 1023;
static bool wide_x, wide_y, was_down;
static int last_x, last_y;
static uint32_t attentions, errors;

static int rd(uint16_t addr, uint8_t *buf, unsigned n)
{
	uint8_t a[2] = { (uint8_t)addr, (uint8_t)(addr >> 8) };
	return i2c_qup_xfer(QUP, ADDR, a, 2, buf, n);
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
	tlmm_irq_ack(GPIO_IRQ);
	tlmm_irq_enable(GPIO_IRQ, true);
}

static void message_done(int err, void *ctx)
{
	struct mxt_touch m;
	if (err)
		errors++;
	else if (msg[0] != 0xff && mxt_palm(&map, msg)) {
		struct touch t = { TOUCH_PALM, last_x, last_y, false, time_ms() };
		was_down = false;
		push(&t);
	} else if (msg[0] != 0xff && mxt_decode(&map, msg, wide_x, wide_y, &m) && !m.finger) {
		struct touch t = { m.down ? (was_down ? TOUCH_MOVE : TOUCH_DOWN) : TOUCH_UP, last_x, last_y, m.down, time_ms() };
		if (m.down) {
			t.x = last_x = (int)((uint32_t)m.x * fb.width / (max_x + 1));
			t.y = last_y = (int)((uint32_t)m.y * fb.panel_height / (max_y + 1));
		}
		if (m.down || was_down)
			push(&t);
		was_down = m.down;
	}
	rearm();
}

static void attention(unsigned pin)
{
	tlmm_irq_enable(GPIO_IRQ, false);
	attentions++;
	if (i2c_qup_start(QUP, ADDR, msg_ptr, 2, msg, map.msg_size, message_done, NULL)) {
		errors++;
		rearm();
	}
}

/* Power, then a reset pulse; the line going low says it is up. */
static bool power_on(void)
{
	rpm_regulator_voltage(RPM_LDOA, 28, 2800000);
	rpm_regulator_enable(RPM_LDOA, 28, true);
	rpm_regulator_sleep_enable(RPM_LDOA, 28, true);
	tlmm_set(GPIO_RESET, 0);
	tlmm_config(GPIO_RESET, 0, TLMM_OUTPUT, TLMM_PULL_NONE, 6);
	delay_ms(2);
	tlmm_set(GPIO_RESET, 1);
	uint32_t t0 = time_ms();
	while (tlmm_get(GPIO_IRQ)) {
		if (time_ms() - t0 > BOOT_WAIT_MS)
			return false;
		wdt_feed();
	}
	return true;
}

static bool mxt_init(void)
{
	tlmm_config(GPIO_IRQ, 0, TLMM_INPUT, TLMM_PULL_UP, 8);
	bool up = power_on();
	uint8_t info[INFO_SIZE], range[4];
	static uint8_t table[MXT_MAX_OBJECTS * MXT_OBJECT_SIZE];
	int ret = i2c_qup_init(QUP, board->touch_pins.i2c.sda, board->touch_pins.i2c.scl, board->touch_pins.i2c.func, BUS_HZ);
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
	tlmm_irq_config(GPIO_IRQ, TLMM_IRQ_LEVEL_LOW, attention);
	tlmm_irq_enable(GPIO_IRQ, true);
	return true;
}

static bool mxt_poll(struct touch *t)
{
	i2c_qup_poll(QUP);
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

const struct touch_driver touch_mxt = { .init = mxt_init, .poll = mxt_poll, .stats = mxt_stats };
