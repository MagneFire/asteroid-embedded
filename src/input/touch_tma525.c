/* Cypress TMA525B on beluga, in its packet protocol over I2C: a report
 * starts with its length, little-endian, and a read from the start gives
 * the whole of it. Report 1 is the touches: their count in byte 5, the
 * records from byte 7, 10 bytes each, the first finger's tip in bit 7 of
 * its byte 1, its event in bits 5-6 (none: nothing to take), and the
 * controller's y then x in bytes 2-5, on a grid over the whole glass
 * that the board can give as smaller than the panel. Stock switches it
 * to its easy wake mode after reset; that reports touches on a coarse
 * grid, so only its switch back before letting go of the controller is
 * done here. The line going low says a report is waiting; the interrupt
 * only marks it, and the read is done from the main loop, where the
 * bus's other users are. */
#include "asteroid.h"
#include "display/display.h"
#include "input/touch.h"
#include "input/tma525_proto.h"
#include "detect.h"

#define BUS		(board->touch_pins.i2c.bus)
#define ADDR		(board->touch_pins.i2c.addr)
#define GPIO_IRQ	(board->touch_pins.irq)
#define GPIO_RESET	(board->touch_pins.reset)
#define REPORT_MAX	107
#define REPORT_TOUCH	1
#define PALM		0x20
#define TIP		0x80
#define EVENT(b)	((b) >> 5 & 3)
#define BOOT_WAIT_MS	500
#define SYSINFO_TRIES	50

static volatile bool attention;
static bool was_down, asleep, lent, up;
static int last_x, last_y;
static uint32_t attentions, errors;
static struct touch pending;
static bool have_pending, sysinfo_done;
static uint16_t grid_w, grid_h;

static void line_low(unsigned pin)
{
	attentions++;
	attention = true;
}

static void normal_mode(void)
{
	const uint8_t cmd[] = { 0x04, 0x00, 0x06, 0x00, 0x2f, 0x00, 0x1b, 0x00 };
	if (i2c_xfer(BUS, ADDR, cmd, sizeof cmd, NULL, 0))
		errors++;
}

/* Other reports that come first are read and dropped. */
static bool read_sysinfo(struct tma525_sysinfo *s)
{
	if (i2c_xfer(BUS, ADDR, tma525_get_sysinfo, sizeof tma525_get_sysinfo, NULL, 0))
		return false;
	uint8_t r[REPORT_MAX];
	for (int i = 0; i < SYSINFO_TRIES; i++) {
		delay_ms(2);
		if (gpio_get(GPIO_IRQ))
			continue;
		if (i2c_xfer(BUS, ADDR, NULL, 0, r, 2))
			return false;
		unsigned n = r[0] | r[1] << 8;
		if (n <= 2)
			continue;
		if (n > REPORT_MAX)
			n = REPORT_MAX;
		if (i2c_xfer(BUS, ADDR, NULL, 0, r, n))
			return false;
		if (tma525_sysinfo_parse(r, n, s))
			return true;
	}
	return false;
}

static void check_size(void)
{
	static const char *const from[] = { [TMA525_GRID_BOARD] = "the board's", [TMA525_GRID_SYSINFO] = "the controller's",
					    [TMA525_GRID_PANEL] = "the panel's" };
	struct tma525_sysinfo s;
	bool have = read_sysinfo(&s);
	if (have) {
		log_printf("touch: TMA525 %ux%u, %ux%u electrodes, panel id %02x, revision %08lx\n", s.res_x, s.res_y,
			   s.electrodes_x, s.electrodes_y, s.panel_id, s.revctrl);
		board_check_touch(&s);
	} else
		log_printf("touch: no system information from the TMA525\n");
	enum tma525_grid_from f = tma525_grid(have ? &s : NULL, board->touch_pins.grid_w, board->touch_pins.grid_h,
					      (uint16_t)fb.width, (uint16_t)fb.height, &grid_w, &grid_h);
	log_printf("touch: grid %ux%u, %s\n", grid_w ? grid_w : (unsigned)fb.width, grid_h ? grid_h : (unsigned)fb.height,
		   from[f]);
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
	delay_ms(10);
	gpio_set(GPIO_RESET, 1);
	int r = i2c_init(BUS, board->touch_pins.i2c.sda, board->touch_pins.i2c.scl, board->touch_pins.i2c.func, 400000);
	uint8_t len[2] = { 0 };
	for (int i = 0; !r; i++) {
		delay_ms(BOOT_WAIT_MS / 10);
		if (!i2c_xfer(BUS, ADDR, NULL, 0, len, 2) && len[0])
			break;
		if (i == 9)
			r = -6;
	}
	up = !r;
	if (r) {
		log_printf("touch: no answer from the TMA525 (%d)\n", r);
		return false;
	}
	log_printf("touch: TMA525 up, first report %u bytes\n", len[0] | len[1] << 8);
	if (!sysinfo_done) {
		sysinfo_done = true;
		check_size();
	}
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
	if (count && n >= 17 && !EVENT(r[8]))
		return false;
	bool down = count && n >= 17 && (r[8] & TIP);
	if (down) {
		int x = r[11] | r[12] << 8, y = r[9] | r[10] << 8;
		if (grid_w)
			x = x * (int)fb.width / grid_w;
		if (grid_h)
			y = y * (int)fb.height / grid_h;
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

/* The line stays low while reports are left, with no edge to wake for
 * them. */
static uint32_t tma_next_ms(void)
{
	return !asleep && (attention || have_pending) ? 1 : UINT32_MAX;
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
	if (up)
		normal_mode();
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
	.init = tma_init, .poll = tma_poll, .sleep = tma_sleep, .stats = tma_stats, .lend = tma_lend, .next_ms = tma_next_ms,
};
