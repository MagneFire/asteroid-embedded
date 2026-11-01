/* ITE IT7260 touch controller at 0x46 on BLSP1 QUP5 (gpio 18/19). The
 * host sees a few buffers, each selected by the first byte of a
 * transfer: commands are written to 0x20 and their responses read from
 * 0xA0; 0x80 is the status byte (bit 0 busy, bit 7 point data waiting);
 * 0xE0 holds the 14-byte point record. A read is the buffer index
 * written, a repeated start, then the data. The interrupt line, gpio
 * 17, falls when a point record is ready.
 *
 * Point record: flags (bits 7-4 record type, 0 for touch; bits 2-0 one
 * per valid finger), palm (bit 0), then three fingers of x low, high
 * nibbles (x in bits 3-0, y in bits 7-4), y low and pressure (bits 3-0,
 * 2 or more is a touch). Coordinates are in the panel's 320 x 320. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "touch.h"

#define QUP		BOARD_TOUCH_QUP
#define ADDR		BOARD_TOUCH_ADDR
#define GPIO_IRQ	BOARD_TOUCH_IRQ
#define GPIO_RESET	BOARD_TOUCH_RESET

#define BUF_COMMAND	0x20
#define BUF_QUERY	0x80
#define BUF_RESPONSE	0xa0
#define BUF_POINT	0xe0

#define QUERY_BUSY	0x01
#define QUERY_POINT	0x80

#define CMD_IDENT	0x00
#define CMD_VERSIONS	0x01
#define VER_FIRMWARE	0x00
#define VER_CONFIG	0x06
#define CMD_PALM	0x21

#define CMD_POWER	0x04
#define POWER_ACTIVE	0x00
#define POWER_IDLE	0x01

#define REC_LEN		14
#define REC_TYPE	0xf0
#define REC_TYPE_WAKE	0x10
#define REC_FINGERS	0x07
#define REC_PALM	0x01
#define PRESSURE_TOUCH	2
#define RANGE		320

/* Command, response and status access, all synchronous (init only). */
static int rd(uint8_t buf_index, uint8_t *buf, unsigned n)
{
	return i2c_qup_xfer(QUP, ADDR, &buf_index, 1, buf, n);
}

/* The controller takes commands only while its busy bit is clear. */
static int wait_ready(unsigned tries, unsigned ms, uint8_t *query)
{
	uint8_t q = 0xff;
	for (unsigned i = 0; i < tries; i++) {
		if (!rd(BUF_QUERY, &q, 1) && !(q & QUERY_BUSY))
			break;
		q = 0xff;
		delay_ms(ms);
		wdt_feed();
	}
	if (query)
		*query = q;
	return q & QUERY_BUSY ? -1 : 0;
}

static int command(const uint8_t *c, unsigned n, uint8_t *resp, unsigned rn)
{
	uint8_t w[12] = { BUF_COMMAND };
	if (n > sizeof(w) - 1)
		return -3;
	memcpy(w + 1, c, n);
	int ret = wait_ready(100, 1, NULL);
	if (!ret)
		ret = i2c_qup_xfer(QUP, ADDR, w, n + 1, NULL, 0);
	if (!ret && rn) {
		ret = wait_ready(100, 1, NULL);
		if (!ret)
			ret = rd(BUF_RESPONSE, resp, rn);
	}
	return ret;
}

/* Power: PM8226 LDO6 (1.8 V) and LDO19 (2.85 V) and LVS1, through the
 * RPM. Reset is active low and only ever pulled down: the line is let
 * go again (input, no pull) rather than driven high. */
#define REG_L6		0x014500
#define REG_L19		0x015200
#define REG_LVS1	0x018000
#define REG_STATUS1	0x08

static void power_on(void)
{
	rpm_regulator_enable(RPM_LDOA, 6, true);
	rpm_regulator_voltage(RPM_LDOA, 19, 2850000);
	rpm_regulator_enable(RPM_LDOA, 19, true);
	rpm_regulator_enable(RPM_VSA, 1, true);
	rpm_regulator_sleep_enable(RPM_LDOA, 6, true);
	rpm_regulator_sleep_enable(RPM_LDOA, 19, true);
	rpm_regulator_sleep_enable(RPM_VSA, 1, true);
	tlmm_set(GPIO_RESET, 0);
	tlmm_config(GPIO_RESET, 0, TLMM_OUTPUT, TLMM_PULL_NONE, 6);
	for (int i = 0; i < 6; i++) {
		delay_ms(10);
		wdt_feed();
	}
	tlmm_config(GPIO_RESET, 0, TLMM_INPUT, TLMM_PULL_NONE, 2);
	for (int i = 0; i < 5; i++) {
		delay_ms(10);
		wdt_feed();
	}
	log_printf("touch: l6 status %02x, l19 status %02x volt %02x.%02x, lvs1 status %02x, reset gpio %u, irq gpio %u\n",
		   pmic_read(REG_L6 + REG_STATUS1), pmic_read(REG_L19 + REG_STATUS1), pmic_read(REG_L19 + 0x40),
		   pmic_read(REG_L19 + 0x41), pmic_read(REG_LVS1 + REG_STATUS1),
		   tlmm_get(GPIO_RESET), tlmm_get(GPIO_IRQ));
}

/* Interrupt handling, all in interrupt context: the falling-edge pin
 * interrupt is masked and acknowledged at once, so an edge during the
 * round stays latched; the status byte is read, then, if a record is
 * waiting, the point record; the decoded event is queued and the pin
 * interrupt unmasked, which fires again at once for a latched edge. */
#define QUEUE_LEN 16
static struct touch queue[QUEUE_LEN];
static volatile unsigned q_head, q_tail;
static volatile bool round_busy;
static volatile uint32_t round_t;
static const uint8_t query_index = BUF_QUERY, point_index = BUF_POINT;
static uint8_t query, rec[REC_LEN];
static uint32_t attentions, errors, rounds, stalls;
static bool asleep, mode_set;
static uint32_t mode_t, mode_wait, mode_rounds;
static unsigned mode_fails;
static bool present, was_down, palm_down;
static int last_x, last_y;

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
	rounds++;
	if (!mode_set)
		mode_wait = 0;
	round_busy = false;
	round_t = time_ms();
	tlmm_irq_enable(GPIO_IRQ, true);
}

static int scale(unsigned v, unsigned size)
{
	int s = (int)(v * size / RANGE);
	return s >= (int)size ? (int)size - 1 : s;
}

/* One finger is reported: the first valid one pressed hard enough. A
 * record with no such finger and no palm lifts it; a palm suppresses
 * fingers until a record with neither. */
static void point_done(int err, void *ctx)
{
	if (err) {
		errors++;
		rearm();
		return;
	}
	if ((rec[0] & REC_TYPE) == REC_TYPE_WAKE) {
		/* The idle mode's wake gesture: a tap where the finger last was. */
		struct touch t = { TOUCH_DOWN, last_x, last_y, true };
		push(&t);
		t.event = TOUCH_UP;
		t.down = false;
		push(&t);
		was_down = false;
		rearm();
		return;
	}
	if (rec[0] & REC_TYPE) {
		rearm();
		return;
	}
	const uint8_t *f = NULL;
	for (unsigned i = 0; i < 3 && !f; i++)
		if ((rec[0] & BIT(i)) && (rec[2 + 4 * i + 3] & 0xf) >= PRESSURE_TOUCH)
			f = rec + 2 + 4 * i;
	bool palm = rec[1] & REC_PALM;
	struct touch t = { TOUCH_NONE, last_x, last_y, false };
	if (palm && !palm_down) {
		if (was_down) {
			t.event = TOUCH_UP;
			push(&t);
			was_down = false;
		}
		t.event = TOUCH_PALM;
		push(&t);
		palm_down = true;
	} else if (palm_down) {
		if (!palm && !f)
			palm_down = false;
	} else if (f) {
		t.x = last_x = scale((unsigned)(f[0] | (f[1] & 0x0f) << 8), fb.width);
		t.y = last_y = scale((unsigned)(f[2] | (f[1] & 0xf0) << 4), fb.height);
		t.event = was_down ? TOUCH_MOVE : TOUCH_DOWN;
		t.down = true;
		was_down = true;
		push(&t);
	} else if (was_down && !palm) {
		t.event = TOUCH_UP;
		was_down = false;
		push(&t);
	}
	rearm();
}

static void query_done(int err, void *ctx)
{
	if (err) {
		errors++;
		rearm();
	} else if (!(query & QUERY_POINT)) {
		rearm();
	} else if (i2c_qup_start(QUP, ADDR, &point_index, 1, rec, REC_LEN, point_done, NULL)) {
		errors++;
		rearm();
	}
}

/* Called with interrupts masked. */
static void start_round(void)
{
	tlmm_irq_enable(GPIO_IRQ, false);
	tlmm_irq_ack(GPIO_IRQ);
	round_busy = true;
	round_t = time_ms();
	attentions++;
	if (i2c_qup_start(QUP, ADDR, &query_index, 1, &query, 1, query_done, NULL)) {
		errors++;
		rearm();
	}
}

static void attention(unsigned pin)
{
	if (!round_busy)
		start_round();
}

static struct {
	bool active, down;
	int x0, y0, x1, y1;
	uint32_t t0, ms, last;
} fake;

void touch_inject(int x0, int y0, int x1, int y1, uint32_t ms)
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
	struct touch t = { TOUCH_DOWN, fake.x0, fake.y0, true };
	if (!fake.down) {
		fake.down = true;
		fake.t0 = fake.last = now;
	} else if (now - fake.t0 >= fake.ms) {
		t = (struct touch){ TOUCH_UP, fake.x1, fake.y1, false };
		fake.active = false;
	} else if (now - fake.last >= 16) {
		int32_t f = (int32_t)((now - fake.t0) * 1024 / fake.ms);
		t = (struct touch){ TOUCH_MOVE, fake.x0 + (fake.x1 - fake.x0) * f / 1024, fake.y0 + (fake.y1 - fake.y0) * f / 1024,
				    true };
		fake.last = now;
	} else {
		return;
	}
	irq_disable();
	push(&t);
	irq_enable();
}

/* An edge missed (the line already low when the interrupt was armed, or
 * a round that failed while the line stayed low) would stop the
 * reports for good, so a line found low with no round for a while
 * starts one from here. */
#define STALL_MS 50

static void stall_poll(void)
{
	if (!present || round_busy || tlmm_get(GPIO_IRQ) || time_ms() - round_t < STALL_MS)
		return;
	uint32_t flags = irq_save();
	if (!round_busy) {
		start_round();
		stalls++;
	}
	irq_restore(flags);
	if (stalls == 1)
		log_printf("touch: irq line low without an interrupt\n");
}

/* Power mode and palm threshold together: active scanning with the
 * normal palm threshold while the screen is on; idle, which reports a
 * wake gesture instead of points, with a lower one while it is off. */
#define PALM_AWAKE	0xa0
#define PALM_ASLEEP	0x70

static int set_mode(uint8_t mode, uint8_t palm)
{
	const uint8_t palm_cmd[] = { CMD_PALM, 0x00, 0x00, palm }, mode_cmd[] = { CMD_POWER, 0x00, mode };
	uint8_t resp[2] = { 0xff, 0xff };
	int ret = command(palm_cmd, sizeof(palm_cmd), resp, sizeof(resp));
	if (!ret && (resp[0] || resp[1]))
		ret = -6;
	if (!ret)
		ret = command(mode_cmd, sizeof(mode_cmd), resp, sizeof(resp));
	if (!ret && (resp[0] || resp[1]))
		ret = -7;
	return ret;
}

/* Called from the main loop: the pin interrupt is held off and a round
 * in flight finished first, then the commands go out synchronously. */

/* The controller stops acknowledging its address some ten seconds
 * after the last touch and only a finger brings it back, so a mode that
 * could not be set is retried a few times and then once more after the
 * next round. The screen-off command itself waits 5 s like the vendor's
 * driver. */
#define IDLE_DELAY_MS	5000
#define RETRY_MS	2000
#define RETRIES		3

static void apply_mode(void)
{
	tlmm_irq_enable(GPIO_IRQ, false);
	uint32_t t0 = time_ms();
	while (round_busy && time_ms() - t0 < 50)
		i2c_qup_poll(QUP);
	uint8_t q = 0xff;
	int ret = rd(BUF_QUERY, &q, 1);
	if (!ret)
		ret = set_mode(asleep ? POWER_IDLE : POWER_ACTIVE, asleep ? PALM_ASLEEP : PALM_AWAKE);
	mode_set = !ret;
	mode_fails = ret ? mode_fails + 1 : 0;
	mode_t = round_t = time_ms();
	mode_wait = RETRY_MS;
	if (!ret || mode_fails == RETRIES)
		log_printf("touch: %s mode %s (%d, try %u)\n", asleep ? "idle" : "active", ret ? "not set" : "set", ret, mode_fails);
	tlmm_irq_enable(GPIO_IRQ, true);
}

void touch_sleep(bool on)
{
	if (!present)
		return;
	asleep = on;
	mode_set = false;
	mode_fails = 0;
	mode_t = time_ms();
	mode_wait = on ? IDLE_DELAY_MS : 0;
}

uint32_t touch_next_ms(void)
{
	if (!present || mode_set || mode_fails >= RETRIES)
		return UINT32_MAX;
	uint32_t gone = time_ms() - mode_t;
	return gone >= mode_wait ? 1 : mode_wait - gone;
}

bool touch_poll(struct touch *t)
{
	i2c_qup_poll(QUP);
	stall_poll();
	if (present && !mode_set && !round_busy && time_ms() - mode_t >= mode_wait) {
		if (rounds != mode_rounds)
			mode_fails = 0;
		mode_rounds = rounds;
		if (mode_fails < RETRIES)
			apply_mode();
	}
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

void touch_stats(uint32_t *att, uint32_t *err)
{
	*att = attentions;
	*err = errors;
}

bool touch_init(void)
{
	tlmm_config(GPIO_IRQ, 0, TLMM_INPUT, TLMM_PULL_UP, 2);
	power_on();
	uint8_t q = 0xff;
	int ret = i2c_qup_init(QUP, BOARD_TOUCH_SDA, BOARD_TOUCH_SCL, 3, 384000);
	if (!ret)
		ret = wait_ready(50, 10, &q);
	if (ret) {
		log_printf("touch: not ready (%d, status %02x)\n", ret, q);
		return false;
	}

	static const uint8_t ident[] = { CMD_IDENT }, expect[] = { 0x0a, 'I', 'T', 'E', '7', '2', '6', '0' };
	uint8_t id[10] = { 0 };
	ret = command(ident, sizeof(ident), id, sizeof(id));
	if (ret || memcmp(id, expect, sizeof(expect))) {
		log_printf("touch: identify failed (%d): %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", ret, id[0], id[1],
			   id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9]);
		return false;
	}
	static const uint8_t fw_cmd[] = { CMD_VERSIONS, VER_FIRMWARE }, cfg_cmd[] = { CMD_VERSIONS, VER_CONFIG };
	uint8_t fw[10] = { 0 }, cfg[10] = { 0 };
	int vret = command(fw_cmd, sizeof(fw_cmd), fw, sizeof(fw));
	if (!vret)
		vret = command(cfg_cmd, sizeof(cfg_cmd), cfg, sizeof(cfg));
	log_printf("touch: IT7260 rev %c%c, firmware %02x %02x %02x %02x, config %02x %02x %02x %02x (%d)\n", id[8], id[9],
		   fw[5], fw[6], fw[7], fw[8], cfg[1], cfg[2], cfg[3], cfg[4], vret);

	ret = set_mode(POWER_ACTIVE, PALM_AWAKE);
	mode_set = !ret;
	log_printf("touch: active mode (%d), status %02x, irq gpio %u\n", ret, q, tlmm_get(GPIO_IRQ));

	tlmm_irq_config(GPIO_IRQ, TLMM_IRQ_EDGE_FALLING, attention);
	round_t = time_ms();
	present = true;
	tlmm_irq_enable(GPIO_IRQ, true);
	return true;
}
