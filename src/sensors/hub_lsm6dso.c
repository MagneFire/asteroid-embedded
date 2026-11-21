/* The LSM6DSO accelerometer on beluga, as the watch's sensor hub: its
 * embedded functions count steps and detect a tilt on their own at
 * 26 Hz, and raise INT1 for a tilt. The interrupt only marks it; the
 * status is read from the main loop. The step counter is 16 bits and
 * is extended here. Readings of the axes are on demand, for turning
 * the screen. */
#include "asteroid.h"
#include "sensors/hub.h"
#include "sensors/afe4900.h"
#include "sensors/lsm6dso.h"

#define BUS		(board->hub_pins.i2c.bus)
#define ADDR		(board->hub_pins.i2c.addr)
#define IRQ		(board->hub_pins.irq)

#define FUNC_CFG_ACCESS	0x01
#define WHO_AM_I	0x0f
#define CTRL1_XL	0x10
#define CTRL2_G		0x11
#define CTRL3_C		0x12
#define CTRL7_G		0x16
#define OUTX_L_G	0x22
#define EMB_STATUS	0x35
#define OUTX_L_A	0x28
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
#define EMB_LIR		0x80
#define IS_TILT		0x10
#define G_2000DPS	0x0c
#define G_LOW_POWER	0x80
/* The datasheet's turn-on time is 70 ms. */
#define G_SETTLE_MS	80

static bool ready, tilt_wanted, accel_wanted;
static volatile bool irq_seen;
static bool tilt;
/* A reset of the MCU leaves the sensor on whichever page it was. */
static bool emb_open = true;
static uint16_t last_count;
static uint32_t steps, errors;
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
 * for readings. */
static void apply(void)
{
	if (main_page())
		return;
	wr(CTRL1_XL, XL_26HZ_4G);
	if (!emb(true)) {
		wr(EMB_FUNC_EN_A, PEDO_EN | (tilt_wanted ? TILT_EN : 0));
		wr(EMB_FUNC_INT1, tilt_wanted ? INT1_TILT : 0);
		wr(PAGE_RW, EMB_LIR);
	}
	if (!emb(false))
		wr(MD1_CFG, tilt_wanted ? INT1_EMB_FUNC : 0);
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

/* The tilt interrupt is latched until its status is read and the pad
 * takes rising edges, so a line still high means a status not read yet,
 * whatever became of the edge. */
static void lsm_poll(void)
{
	afe4900_poll();
	if (!ready || (!irq_seen && !(tilt_wanted && gpio_get(IRQ))))
		return;
	irq_seen = false;
	uint8_t st;
	if (!main_page() && !rd(EMB_STATUS, &st, 1) && (st & IS_TILT))
		tilt = true;
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
	apply();
	return true;
}

static void lsm_accel_on(bool on) { accel_wanted = on; }

/* In milli-g at the 4 g range, 0.122 mg a count. */
static bool lsm_accel(int16_t v[3])
{
	uint8_t raw[6];
	if (!ready || !accel_wanted || main_page() || rd(OUTX_L_A, raw, 6))
		return false;
	for (int i = 0; i < 3; i++)
		v[i] = (int16_t)((int16_t)(raw[2 * i] | raw[2 * i + 1] << 8) * 122 / 1000);
	return true;
}

void lsm6dso_gyro_on(uint32_t period_ms)
{
	uint8_t odr = lsm6dso_gyro_odr(period_ms);
	if (!ready || odr == gyro_odr || main_page())
		return;
	if (wr(CTRL7_G, G_LOW_POWER) || wr(CTRL2_G, (uint8_t)(odr << 4 | (odr ? G_2000DPS : 0))))
		return;
	gyro_odr = odr;
	gyro_t = time_ms();
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

static bool lsm_can_sleep(void) { return !irq_seen && afe4900_can_sleep(); }
static void lsm_sleep(bool asleep) {}

static void lsm_status(void (*say)(const char *fmt, ...))
{
	say("hub: LSM6DSO %s, %lu steps, tilt %s, gyroscope ODR %u, %lu errors\n", ready ? "up" : "absent", steps,
	    tilt_wanted ? "on" : "off", gyro_odr, errors);
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
