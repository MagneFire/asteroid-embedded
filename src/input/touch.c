/* Synaptics RMI4 touch controller at 0x20 on BLSP1 QUP5 (gpio 18/19).
 * The function table on
 * page 0 starts at 0xE9 and grows downwards, six bytes per entry (query,
 * command, control and data base, interrupt count, function number); it
 * is scanned for F01 (device control: status, interrupt status) and the
 * 2D sensor, F11 (finger state byte then 5 bytes per finger: x and y high
 * bytes, low nibbles, width, pressure) or F12 (8 bytes per object: type,
 * x, y, z, wx, wy). The attention line, gpio 17, is low while a report is
 * pending and raises a pin interrupt. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "touch.h"

#define QUP		5
#define F11_CTRL58	0x64
#define PALM_SIZE	40
#define ADDR		0x20
#define PDT_START	0xe9
#define PAGE_SELECT	0xff
#define GPIO_ATTN	17
#define GPIO_RESET	16

struct fn {
	uint8_t query, cmd, ctrl, data, intr, number;
};
static struct fn f01, f11, f12;
static bool present, was_down, palm_down;
static int last_x, last_y;
static unsigned status_bytes = 1, max_x = 4095, max_y = 4095;

static int rd(uint8_t reg, uint8_t *buf, unsigned n)
{
	return i2c_qup_xfer(QUP, ADDR, &reg, 1, buf, n);
}

static int wr(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return i2c_qup_xfer(QUP, ADDR, buf, 2, NULL, 0);
}

/* Power: analog supply from PM8226 LDO19, the I2C pull-ups from LVS1,
 * both enabled through the RPM, then a reset pulse on gpio 16. */
#define REG_L19		0x015200
#define REG_LVS1	0x018000
#define REG_STATUS1	0x08
#define REG_VOLTAGE1	0x40
#define REG_VOLTAGE2	0x41
#define REG_EN_CTL	0x46

static void power_on(void)
{
	rpm_regulator_enable(RPM_LDOA, 19, true);
	rpm_regulator_enable(RPM_VSA, 1, true);
	tlmm_set(GPIO_RESET, 0);
	tlmm_config(GPIO_RESET, 0, TLMM_OUTPUT, TLMM_PULL_NONE, 2);
	for (int i = 0; i < 16; i++) {
		delay_ms(10);
		wdt_feed();
	}
	tlmm_set(GPIO_RESET, 1);
	for (int i = 0; i < 15; i++) {
		delay_ms(10);
		wdt_feed();
	}
	log_printf("touch: sid1 rev %02x, l19 status %02x volt %02x.%02x, lvs1 status %02x, reset gpio %u\n",
		   pmic_read(0x010104), pmic_read(REG_L19 + REG_STATUS1), pmic_read(REG_L19 + REG_VOLTAGE1),
		   pmic_read(REG_L19 + REG_VOLTAGE2), pmic_read(REG_LVS1 + REG_STATUS1), tlmm_get(GPIO_RESET));
}

/* Attention handling, all in interrupt context: the level-low pin
 * interrupt is masked, the interrupt status of F01 is read (which
 * releases the line), then the finger data; the decoded event is queued
 * and the pin interrupt unmasked again, so a line still held low starts
 * the next round at once. */
#define QUEUE_LEN 16
static struct touch queue[QUEUE_LEN];
static volatile unsigned q_head, q_tail;
static uint8_t irq_status[2], report[16], data_reg, data_len;
static uint32_t attentions, errors;
static uint8_t status_reg, palm_reg, palm_data;

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
	tlmm_irq_ack(GPIO_ATTN);
	tlmm_irq_enable(GPIO_ATTN, true);
}

static void palm_done(int err, void *ctx)
{
	bool palm = !err && (palm_data & 2);
	if (palm && !palm_down) {
		struct touch t = { TOUCH_PALM, last_x, last_y, false };
		push(&t);
	}
	palm_down = palm;
	rearm();
}

static void report_done(int err, void *ctx)
{
	if (err) {
		errors++;
		rearm();
		return;
	}
	struct touch t = { TOUCH_NONE, 0, 0, false };
	int x, y;
	bool down;
	if (f11.number) {
		const uint8_t *f = report + status_bytes;
		down = (report[0] & 3) != 0;
		x = (f[0] << 4 | (f[2] & 0xf)) * (int)fb.width / (int)(max_x + 1);
		y = (f[1] << 4 | f[2] >> 4) * (int)fb.height / (int)(max_y + 1);
	} else {
		down = report[0] != 0;
		x = report[1] | report[2] << 8;
		y = report[3] | report[4] << 8;
	}
	if (down) {
		t.event = was_down ? TOUCH_MOVE : TOUCH_DOWN;
		last_x = x;
		last_y = y;
	} else if (was_down) {
		t.event = TOUCH_UP;
		x = last_x;
		y = last_y;
	}
	t.x = x;
	t.y = y;
	t.down = down;
	was_down = down;
	if (t.event != TOUCH_NONE)
		push(&t);
	if (!palm_reg || i2c_qup_start(QUP, ADDR, &palm_reg, 1, &palm_data, 1, palm_done, NULL))
		rearm();
}

/* Large object suppression flags a palm in F11 data 28; its offset follows
 * from which F11 queries and data registers the sensor has. */
static unsigned f11_data28(void)
{
	uint8_t q[6], q78[2] = { 0, 0 }, q9 = 0;
	rd(f11.query, q, 6);
	unsigned off = 6;
	bool rel = q[1] & 0x08, gestures = q[1] & 0x20;
	if (rel)
		off += 1;
	if (gestures) {
		rd(f11.query + off, q78, 2);
		off += 2;
	}
	if (q[0] & 0x08)
		rd(f11.query + off, &q9, 1);
	if (!(q[5] & 0x60))
		return 0;
	unsigned fingers = (q[1] & 7) <= 4 ? (q[1] & 7) + 1 : 10, d = (fingers + 3) / 4 + 5 * fingers;
	if (rel)
		d += 2 * fingers;
	if (gestures) {
		if (q78[0])
			d += 1;
		if (q78[0] || q78[1])
			d += 1;
		if (q78[0] & 0x50)
			d += 1;
		if ((q78[0] & 0x10) || (q78[1] & 0x02))
			d += 2;
		if (q78[1] & 0x04)
			d += (fingers + 3) / 4;
		if ((q78[1] & 0x08) || (q78[1] & 0x20) || (q78[0] & 0x80))
			d += 2;
		if ((q78[1] & 0x08) && (q78[1] & 0x10))
			d += 2;
	}
	if ((q[0] & 0x08) && (q9 & 0x20))
		d += 10 * fingers;
	return d;
}

static void status_done(int err, void *ctx)
{
	if (err || i2c_qup_start(QUP, ADDR, &data_reg, 1, report, data_len, report_done, NULL)) {
		errors++;
		rearm();
	}
}

static void attention(unsigned pin)
{
	tlmm_irq_enable(GPIO_ATTN, false);
	attentions++;
	if (i2c_qup_start(QUP, ADDR, &status_reg, 1, irq_status, 2, status_done, NULL)) {
		errors++;
		rearm();
	}
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

bool touch_poll(struct touch *t)
{
	i2c_qup_poll(QUP);
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
	tlmm_config(GPIO_ATTN, 0, TLMM_INPUT, TLMM_PULL_UP, 2);
	power_on();
	uint8_t e[6];
	int ret = i2c_qup_init(QUP, 18, 19, 3, 384000);
	if (!ret)
		ret = rd(PAGE_SELECT, e, 1);
	if (ret) {
		log_printf("touch: no answer on i2c (%d)\n", ret);
		return false;
	}

	wr(PAGE_SELECT, 0);
	for (uint8_t a = PDT_START; a > 0x20; a -= 6) {
		if (rd(a, e, 6))
			return false;
		struct fn f = { e[0], e[1], e[2], e[3], e[4] & 7, e[5] };
		if (!f.number || f.number == 0xff)
			break;
		log_printf("touch: F%02x query %02x cmd %02x ctrl %02x data %02x irqs %u\n",
			   f.number, f.query, f.cmd, f.ctrl, f.data, f.intr);
		if (f.number == 0x01)
			f01 = f;
		else if (f.number == 0x11)
			f11 = f;
		else if (f.number == 0x12)
			f12 = f;
	}
	if (!f01.number || (!f11.number && !f12.number)) {
		log_printf("touch: F01 or 2D sensor not found\n");
		return false;
	}
	uint8_t id[11] = { 0 };
	rd(f01.query + 11, id, 10);
	uint8_t st[2];
	rd(f01.data, st, 2);
	if (f11.number) {
		uint8_t q[2], c[4];
		rd(f11.query, q, 2);
		unsigned fingers = (q[1] & 7) <= 4 ? (q[1] & 7) + 1 : 10;
		status_bytes = (fingers + 3) / 4;
		rd(f11.ctrl + 6, c, 4);
		max_x = c[0] | (c[1] & 0xf) << 8;
		max_y = c[2] | (c[3] & 0xf) << 8;
		log_printf("touch: F11 %u fingers, %u status bytes, max %u x %u\n", fingers, status_bytes, max_x, max_y);
	}
	if (f11.number) {
		unsigned d = f11_data28();
		if (d && !wr(F11_CTRL58, PALM_SIZE | 0x80))
			palm_reg = (uint8_t)(f11.data + d);
		log_printf("touch: palm detection %s, data 28 at +%u\n", palm_reg ? "on" : "off", d);
	}
	log_printf("touch: product %s, status %02x irq %02x, attn %u\n", (char *)id, st[0], st[1], tlmm_get(GPIO_ATTN));
	status_reg = f01.data;
	data_reg = f11.number ? f11.data : f12.data;
	data_len = f11.number ? (uint8_t)(status_bytes + 5) : 8;
	tlmm_irq_config(GPIO_ATTN, TLMM_IRQ_LEVEL_LOW, attention);
	tlmm_irq_enable(GPIO_ATTN, true);
	present = true;
	return true;
}
