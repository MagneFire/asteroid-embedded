/* The LSM6DSO accelerometer on beluga, as the watch's sensor hub: its
 * embedded functions count steps on their own at 26 Hz. For wrist
 * raises, as the vendor's firmware has it, the accelerometer's samples
 * go through its FIFO at ±8 g, with an interrupt on INT1 for each, and
 * are taken in order from the main loop, so none is missed between two
 * passes. The step counter is 16 bits and is extended here. Readings
 * for turning the screen are the latest sample while the FIFO runs. */
#include "asteroid.h"
#include "sensors/hub.h"
#include "sensors/afe4900.h"
#include "sensors/lsm6dso.h"
#include "sensors/raise.h"

#define BUS		(board->hub_pins.i2c.bus)
#define ADDR		(board->hub_pins.i2c.addr)
#define IRQ		(board->hub_pins.irq)

#define FUNC_CFG_ACCESS	0x01
#define FIFO_CTRL1	0x07
#define FIFO_CTRL2	0x08
#define FIFO_CTRL3	0x09
#define FIFO_CTRL4	0x0a
#define INT1_CTRL	0x0d
#define WHO_AM_I	0x0f
#define CTRL1_XL	0x10
#define CTRL2_G		0x11
#define CTRL3_C		0x12
#define CTRL7_G		0x16
#define OUTX_L_G	0x22
#define OUTX_L_A	0x28
#define FIFO_STATUS1	0x3a
#define FIFO_DATA_OUT	0x78
/* In the embedded functions' page. */
#define EMB_FUNC_EN_A	0x04
#define STEP_COUNTER_L	0x62

#define ID		0x6c
#define EMB_PAGE	0x80
#define XL_26HZ_8G	0x2c
#define BDU_INC		0x44
#define SW_RESET	0x01
#define PEDO_EN		0x08
#define FIFO_WTM_ONE	0x01
#define BDR_XL_26HZ	0x02
#define FIFO_CONTINUOUS	0x06
#define INT1_FIFO_TH	0x08
#define FIFO_OVR_LATCHED	0x08
#define DIFF_FIFO_HI	0x03
#define TAG_XL		0x02
#define G_2000DPS	0x0c
#define G_LOW_POWER	0x80
/* The datasheet's turn-on time is 70 ms. */
#define G_SETTLE_MS	80
#define UMG_PER_COUNT	244
/* Samples taken in one pass; the FIFO holds 20 s of them. */
#define DRAIN_MAX	32
/* With the AP's screen, which the hub is not told of: taken as on from
 * a raise, until a lower or this long. */
#define GUESS_MS	10000

static bool ready, tilt_wanted, fifo_on, restarted, accel_wanted, have_latest;
static volatile bool irq_seen;
static bool tilt;
static struct raise raise;
static int screen_mode = HUB_SCREEN_OFF;
static bool screen_on;
static uint32_t guess_t, fed_t;
static int16_t latest[3];
/* A reset of the MCU leaves the sensor on whichever page it was. */
static bool emb_open = true;
static uint16_t last_count;
static uint32_t steps, errors, samples, raises, lowers, overruns;
static uint8_t gyro_odr;
static uint32_t gyro_t;

static int wr(uint8_t reg, uint8_t v)
{
	uint8_t w[2] = { reg, v };
	int r = i2c_xfer(BUS, ADDR, w, 2, NULL, 0);
	errors += r != 0;
	return r;
}

static int rd(uint8_t reg, uint8_t *p, unsigned n)
{
	int r = i2c_xfer(BUS, ADDR, &reg, 1, p, n);
	errors += r != 0;
	return r;
}

/* A failed switch may still have landed, so the embedded page counts as
 * selected until a switch back is acknowledged, and every access to the
 * main page retries that first. */
static int emb(bool on)
{
	int r = wr(FUNC_CFG_ACCESS, on ? EMB_PAGE : 0);
	if (on || !r)
		emb_open = on;
	return r;
}

static int main_page(void) { return emb_open ? emb(false) : 0; }

static void line_up(unsigned pin) { irq_seen = true; }

/* Bypass empties the FIFO; false if any write failed. */
static bool apply(void)
{
	if (main_page())
		return false;
	int e = wr(FIFO_CTRL1, tilt_wanted ? FIFO_WTM_ONE : 0);
	e |= wr(FIFO_CTRL2, 0);
	e |= wr(FIFO_CTRL3, tilt_wanted ? BDR_XL_26HZ : 0);
	e |= wr(FIFO_CTRL4, tilt_wanted ? FIFO_CONTINUOUS : 0);
	e |= wr(INT1_CTRL, tilt_wanted ? INT1_FIFO_TH : 0);
	if (!e && tilt_wanted && !fifo_on)
		restarted = true;
	fifo_on = !e && tilt_wanted;
	have_latest &= fifo_on;
	return !e;
}

static bool lsm_init(void)
{
	const struct board_i2c *i = &board->hub_pins.i2c;
	if (i2c_init(i->bus, i->sda, i->scl, i->func, 400000))
		return false;
	uint8_t id = 0;
	if (main_page() || rd(WHO_AM_I, &id, 1) || id != ID) {
		log_printf("hub: no LSM6DSO (id %02x)\n", id);
		return false;
	}
	wr(CTRL3_C, SW_RESET);
	delay_ms(5);
	wr(CTRL3_C, BDU_INC);
	wr(CTRL1_XL, XL_26HZ_8G);
	if (!emb(true))
		wr(EMB_FUNC_EN_A, PEDO_EN);
	emb(false);
	raise_init(&raise);
	apply();
	gpio_config(IRQ, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	gpio_irq_config(IRQ, GPIO_IRQ_EDGE_RISING, line_up);
	gpio_irq_enable(IRQ, true);
	ready = true;
	log_printf("hub: LSM6DSO up\n");
	return true;
}

static void read_steps(void)
{
	uint8_t c[2];
	int r = main_page() || emb(true) || rd(STEP_COUNTER_L, c, 2);
	emb(false);
	if (r)
		return;
	uint16_t count = (uint16_t)(c[0] | c[1] << 8);
	steps += (uint16_t)(count - last_count);
	last_count = count;
}

static void to_mg(const uint8_t *raw, int16_t v[3])
{
	for (int i = 0; i < 3; i++)
		v[i] = (int16_t)((int16_t)(raw[2 * i] | raw[2 * i + 1] << 8) * UMG_PER_COUNT / 1000);
}

static bool read_accel(int16_t v[3])
{
	uint8_t raw[6];
	if (main_page() || rd(OUTX_L_A, raw, 6))
		return false;
	to_mg(raw, v);
	return true;
}

static void set_screen(bool on)
{
	if (on != screen_on)
		raise_screen(&raise, screen_on = on);
}

/* The first sample after the FIFO was off stands for the time it was,
 * as the vendor's firmware feeds it. */
static void feed(const uint8_t *raw, uint32_t now)
{
	float a[3];
	for (int i = 0; i < 3; i++)
		a[i] = (float)(int16_t)(raw[2 * i] | raw[2 * i + 1] << 8) * (UMG_PER_COUNT * 9.8f / 1000000);
	unsigned n = restarted && samples ? raise_repeats((int32_t)(now - fed_t)) : 1;
	restarted = false;
	fed_t = now;
	samples++;
	if (screen_mode == HUB_SCREEN_UNKNOWN && screen_on && now - guess_t >= GUESS_MS)
		set_screen(false);
	while (n--) {
		int e = raise_feed(&raise, a, screen_on);
		if (e == RAISE_UP) {
			raises++;
			tilt = true;
			guess_t = now;
			if (screen_mode == HUB_SCREEN_UNKNOWN)
				set_screen(true);
		} else if (e == RAISE_DOWN) {
			lowers++;
			if (screen_mode == HUB_SCREEN_UNKNOWN)
				set_screen(false);
		}
	}
}

static void drain(uint32_t now)
{
	uint8_t st[2], w[7];
	if (main_page() || rd(FIFO_STATUS1, st, 2))
		return;
	overruns += !!(st[1] & FIFO_OVR_LATCHED);
	unsigned n = st[0] | (st[1] & DIFF_FIFO_HI) << 8;
	for (unsigned k = 0; k < n && k < DRAIN_MAX && !rd(FIFO_DATA_OUT, w, sizeof w); k++) {
		if (w[0] >> 3 != TAG_XL)
			continue;
		to_mg(w + 1, latest);
		have_latest = true;
		feed(w + 1, now);
	}
}

/* The watermark's level stays up while a sample is left and the pad
 * takes rising edges, so a line still high means samples not taken
 * yet, whatever became of the edge. */
static void lsm_poll(void)
{
	afe4900_poll();
	if (!ready)
		return;
	if (irq_seen || (fifo_on && gpio_get(IRQ))) {
		irq_seen = false;
		if (fifo_on)
			drain(time_ms());
	}
}

static uint32_t lsm_steps(void)
{
	if (ready)
		read_steps();
	return ready ? steps : HUB_NONE;
}

static bool lsm_tilted(void)
{
	bool t = tilt;
	tilt = false;
	return t;
}

static bool lsm_tilt_on(bool on)
{
	if (!ready)
		return false;
	tilt_wanted = on;
	return apply();
}

static void lsm_tilt_screen(int mode)
{
	screen_mode = mode;
	if (mode != HUB_SCREEN_UNKNOWN)
		set_screen(mode == HUB_SCREEN_ON);
}

static void lsm_accel_on(bool on) { accel_wanted = on; }

static bool lsm_accel(int16_t v[3])
{
	if (!ready || !accel_wanted)
		return false;
	if (!have_latest)
		return read_accel(v);
	for (int i = 0; i < 3; i++)
		v[i] = latest[i];
	return true;
}

bool lsm6dso_gyro_on(uint32_t period_ms)
{
	uint8_t odr = lsm6dso_gyro_odr(period_ms);
	if (!ready || odr == gyro_odr)
		return true;
	if (main_page() || wr(CTRL7_G, G_LOW_POWER) || wr(CTRL2_G, (uint8_t)(odr << 4 | (odr ? G_2000DPS : 0))))
		return false;
	gyro_odr = odr;
	gyro_t = time_ms();
	return true;
}

bool lsm6dso_gyro(int16_t v[3])
{
	uint8_t raw[6];
	if (!gyro_odr || time_ms() - gyro_t < G_SETTLE_MS || main_page() || rd(OUTX_L_G, raw, 6))
		return false;
	for (int i = 0; i < 3; i++)
		v[i] = (int16_t)(raw[2 * i] | raw[2 * i + 1] << 8);
	return true;
}

static void lsm_power_down(bool down)
{
	if (ready && !main_page())
		wr(CTRL1_XL, down ? 0 : XL_26HZ_8G);
}

static bool lsm_can_sleep(void) { return !irq_seen && !(fifo_on && gpio_get(IRQ)) && afe4900_can_sleep(); }
static void lsm_sleep(bool asleep) {}

static void lsm_status(void (*say)(const char *fmt, ...))
{
	static const char *const modes[] = { "off", "on", "the AP's" };
	say("hub: LSM6DSO %s, %lu steps, raise %s (screen %s, taken as %s; %lu samples, %lu raises, %lu lowers, "
	    "%lu overruns), gyroscope ODR %u, %lu errors\n",
	    ready ? "up" : "absent", steps, fifo_on ? "on" : "off", modes[screen_mode], screen_on ? "on" : "off", samples,
	    raises, lowers, overruns, gyro_odr, errors);
	afe4900_status(say);
}

static int lsm_raw(uint8_t bank, uint8_t offset, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	if (!ready || main_page() || (bank && emb(true)))
		return -1;
	int e = 0;
	for (unsigned i = 0; i < wn && !e; i++)
		e = wr((uint8_t)(offset + i), w[i]);
	if (!e && rn)
		e = rd(offset, r, rn);
	if (bank)
		emb(false);
	return e;
}

const struct hub_driver hub_lsm6dso = {
	.init = lsm_init, .poll = lsm_poll, .steps = lsm_steps, .tilted = lsm_tilted, .tilt_on = lsm_tilt_on,
	.tilt_screen = lsm_tilt_screen,
	.accel_on = lsm_accel_on, .accel = lsm_accel, .can_sleep = lsm_can_sleep, .sleep = lsm_sleep,
	.power_down = lsm_power_down,
	.status = lsm_status, .raw = lsm_raw, .heart_rate_on = afe4900_on, .heart_rate = afe4900_rate,
};
