/* Cypress TMA525B on beluga, in its packet protocol over I2C: a report
 * starts with its length, little-endian, and a read from the start gives
 * the whole of it. Report 1 is the touches: their count in byte 5, the
 * records from byte 7, 10 bytes each, the first finger's tip in bit 7 of
 * its byte 1 and x, y in bytes 2-5 in panel pixels. The line going low
 * says a report is waiting; the interrupt only marks it, and the read
 * is done from the main loop, where the bus's other users are. */
#include "asteroid.h"
#include "display/display.h"
#include "input/touch.h"

#define BUS		(board->touch_pins.i2c.bus)
#define ADDR		(board->touch_pins.i2c.addr)
#define GPIO_IRQ	(board->touch_pins.irq)
#define GPIO_RESET	(board->touch_pins.reset)
#define REPORT_MAX	107
#define REPORT_TOUCH	1
#define PALM		0x20
#define TIP		0x80
#define BOOT_WAIT_MS	200

static volatile bool attention;
static bool was_down, asleep, lent;
static int last_x, last_y;
static uint32_t attentions, errors;
static struct touch pending;
static bool have_pending;

static void line_low(unsigned pin)
{
	attentions++;
	attention = true;
}

static bool tma_init(void)
{
	if (board->touch_pins.power_gpio) {
		gpio_set(board->touch_pins.power_gpio, 1);
		gpio_config(board->touch_pins.power_gpio, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	}
	gpio_config(GPIO_IRQ, 0, GPIO_INPUT, GPIO_PULL_UP, 2);
	gpio_set(GPIO_RESET, 1);
	gpio_config(GPIO_RESET, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	delay_ms(2);
	gpio_set(GPIO_RESET, 0);
	delay_ms(1);
	gpio_set(GPIO_RESET, 1);
	int r = i2c_init(BUS, board->touch_pins.i2c.sda, board->touch_pins.i2c.scl, board->touch_pins.i2c.func, 400000);
	uint8_t len[2] = { 0 };
	for (int i = 0; !r && i < 10; i++) {
		delay_ms(BOOT_WAIT_MS / 10);
		r = i2c_xfer(BUS, ADDR, NULL, 0, len, 2);
	}
	if (r) {
		log_printf("touch: no answer from the TMA525 (%d)\n", r);
		return false;
	}
	log_printf("touch: TMA525 up, first report %u bytes\n", len[0] | len[1] << 8);
	gpio_irq_config(GPIO_IRQ, GPIO_IRQ_EDGE_FALLING, line_low);
	gpio_irq_enable(GPIO_IRQ, true);
	attention = !gpio_get(GPIO_IRQ);
	return true;
}

static bool decode(const uint8_t *r, unsigned n, struct touch *t)
{
	if (n < 7 || r[2] != REPORT_TOUCH)
		return false;
	unsigned count = r[5] & 0x1f;
	uint32_t now = time_ms();
	if (!count && (r[5] & PALM)) {
		*t = (struct touch){ TOUCH_PALM, last_x, last_y, false, now };
		was_down = false;
		return true;
	}
	bool down = count && n >= 17 && (r[8] & TIP);
	if (down) {
		int x = r[9] | r[10] << 8, y = r[11] | r[12] << 8;
		last_x = x < (int)fb.width ? x : (int)fb.width - 1;
		last_y = y < (int)fb.height ? y : (int)fb.height - 1;
	}
	if (!down && !was_down)
		return false;
	*t = (struct touch){ down ? (was_down ? TOUCH_MOVE : TOUCH_DOWN) : TOUCH_UP, last_x, last_y, down, now };
	was_down = down;
	return true;
}

static bool tma_poll(struct touch *t)
{
	if (lent)
		return false;
	if (have_pending) {
		*t = pending;
		have_pending = false;
		return true;
	}
	if (!attention && gpio_get(GPIO_IRQ))
		return false;
	attention = false;
	uint8_t r[REPORT_MAX];
	if (i2c_xfer(BUS, ADDR, NULL, 0, r, 2)) {
		errors++;
		return false;
	}
	unsigned n = r[0] | r[1] << 8;
	if (n <= 2)
		return false;
	if (n > REPORT_MAX)
		n = REPORT_MAX;
	if (i2c_xfer(BUS, ADDR, NULL, 0, r, n)) {
		errors++;
		return false;
	}
	if (!gpio_get(GPIO_IRQ))
		attention = true;
	return decode(r, n, t);
}

static void tma_sleep(bool on, bool wakes)
{
	asleep = on;
}

/* As the vendor's firmware lets go of it: reset and interrupt lines
 * undriven and without pull. */
static void tma_lend(bool to_ap)
{
	if (to_ap == lent)
		return;
	lent = to_ap;
	if (!to_ap) {
		tma_init();
		return;
	}
	gpio_irq_enable(GPIO_IRQ, false);
	gpio_config(GPIO_RESET, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	gpio_config(GPIO_IRQ, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	attention = have_pending = was_down = false;
}

static void tma_stats(uint32_t *att, uint32_t *err)
{
	*att = attentions;
	*err = errors;
}

const struct touch_driver touch_tma525 = {
	.init = tma_init, .poll = tma_poll, .sleep = tma_sleep, .stats = tma_stats, .lend = tma_lend,
};
