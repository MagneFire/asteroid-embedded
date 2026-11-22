/* The LSM6DSO accelerometer on beluga, as the watch's sensor hub: its
 * embedded functions count steps on their own at 26 Hz. For a wrist
 * raise, a change of its 6D position or an embedded tilt raises INT1,
 * and the samples of the next moments tell whether it was a raise. The
 * interrupt only marks it; the rest is done from the main loop. The
 * step counter is 16 bits and is extended here. Readings of the axes
 * are on demand, for turning the screen. */
#include "asteroid.h"
#include "sensors/hub.h"
#include "sensors/afe4900.h"
#include "sensors/lsm6dso.h"
#include "sensors/raise.h"

#define BUS		(board->hub_pins.i2c.bus)
#define ADDR		(board->hub_pins.i2c.addr)
#define IRQ		(board->hub_pins.irq)

#define FUNC_CFG_ACCESS	0x01
#define WHO_AM_I	0x0f
#define CTRL1_XL	0x10
#define CTRL2_G		0x11
#define CTRL3_C		0x12
#define CTRL7_G		0x16
#define D6D_SRC		0x1d
#define STATUS_REG	0x1e
#define OUTX_L_G	0x22
#define EMB_STATUS	0x35
#define OUTX_L_A	0x28
#define TAP_CFG0	0x56
#define TAP_CFG2	0x58
#define TAP_THS_6D	0x59
#define MD1_CFG		0x5e
/* In the embedded functions' page. */
#define EMB_FUNC_EN_A	0x04
#define EMB_FUNC_INT1	0x0a
#define PAGE_RW		0x17
#define STEP_COUNTER_L	0x62

#define ID		0x6c
#define EMB_PAGE	0x80
#define XL_26HZ_4G	0x28
#define XL_OFF		0x00
#define BDU_INC		0x44
#define SW_RESET	0x01
#define PEDO_EN		0x08
#define TILT_EN		0x10
#define INT1_TILT	0x10
#define INT1_EMB_FUNC	0x02
#define INT1_6D		0x04
#define EMB_LIR		0x80
#define IS_TILT		0x10
#define LIR_CLR_ON_READ	0x41
#define INTERRUPTS_EN	0x80
#define SIXD_50_DEG	0x60
#define D6D_IA		0x40
#define XLDA		0x01
#define G_2000DPS	0x0c
#define G_LOW_POWER	0x80
/* The datasheet's turn-on time is 70 ms. */
#define G_SETTLE_MS	80

static bool ready, tilt_wanted, accel_wanted;
static volatile bool irq_seen;
static bool tilt;
static struct raise raise;
/* A reset of the MCU leaves the sensor on whichever page it was. */
static bool emb_open = true;
static uint16_t last_count;
static uint32_t steps, errors, turns, raises;
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

/* The accelerometer runs while the step counter or tilt needs it, and
 * for readings. The embedded tilt stays on beside the 6D position, for
 * raises that end between two of its positions. False if any write
 * failed. */
static bool apply(void)
{
	if (main_page())
		return false;
	int e = wr(CTRL1_XL, XL_26HZ_4G);
	e |= wr(TAP_CFG0, LIR_CLR_ON_READ);
	e |= wr(TAP_THS_6D, SIXD_50_DEG);
	e |= wr(TAP_CFG2, tilt_wanted ? INTERRUPTS_EN : 0);
	e |= emb(true);
	if (!e) {
		e |= wr(EMB_FUNC_EN_A, PEDO_EN | (tilt_wanted ? TILT_EN : 0));
		e |= wr(EMB_FUNC_INT1, tilt_wanted ? INT1_TILT : 0);
		e |= wr(PAGE_RW, EMB_LIR);
	}
	if (emb(false))
		return false;
	e |= wr(MD1_CFG, tilt_wanted ? INT1_EMB_FUNC | INT1_6D : 0);
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

static bool read_accel(int16_t v[3])
{
	uint8_t raw[6];
	if (main_page() || rd(OUTX_L_A, raw, 6))
		return false;
	for (int i = 0; i < 3; i++)
		v[i] = (int16_t)((int16_t)(raw[2 * i] | raw[2 * i + 1] << 8) * 122 / 1000);
	return true;
}

/* Both interrupts are latched until their status is read and the pad
 * takes rising edges, so a line still high means a status not read yet,
 * whatever became of the edge. */
static void lsm_poll(void)
{
	afe4900_poll();
	if (!ready)
		return;
	uint32_t now = time_ms();
	if (irq_seen || (tilt_wanted && gpio_get(IRQ))) {
		irq_seen = false;
		uint8_t st = 0, d6d = 0;
		if (!main_page()) {
			rd(EMB_STATUS, &st, 1);
			rd(D6D_SRC, &d6d, 1);
		}
		if (tilt_wanted && ((st & IS_TILT) || (d6d & D6D_IA))) {
			turns++;
			raise_check(&raise, now);
		}
	}
	uint8_t xl;
	int16_t v[3];
	if (tilt_wanted && raise_checking(&raise, now) && !main_page() && !rd(STATUS_REG, &xl, 1) && (xl & XLDA) &&
	    read_accel(v) && raise_sample(&raise, v, now)) {
		raises++;
		tilt = true;
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
	if (on && !tilt_wanted)
		raise_arm(&raise, time_ms());
	tilt_wanted = on;
	return apply();
}

static void lsm_accel_on(bool on) { accel_wanted = on; }

/* In milli-g at the 4 g range, 0.122 mg a count. */
static bool lsm_accel(int16_t v[3]) { return ready && accel_wanted && read_accel(v); }

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

/* A raise being checked needs a pass for each sample. */
static bool lsm_can_sleep(void) { return !irq_seen && !(tilt_wanted && raise.checking) && afe4900_can_sleep(); }
static void lsm_sleep(bool asleep) {}

static void lsm_status(void (*say)(const char *fmt, ...))
{
	say("hub: LSM6DSO %s, %lu steps, raise %s (%lu turns, %lu raises, face %s), gyroscope ODR %u, %lu errors\n",
	    ready ? "up" : "absent", steps, tilt_wanted ? "on" : "off", turns, raises, raise.in_view ? "up" : "away",
	    gyro_odr, errors);
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
	.accel_on = lsm_accel_on, .accel = lsm_accel, .can_sleep = lsm_can_sleep, .sleep = lsm_sleep,
	.status = lsm_status, .raw = lsm_raw, .heart_rate_on = afe4900_on, .heart_rate = afe4900_rate,
};
