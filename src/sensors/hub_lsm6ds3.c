/* swift's motion sensor without a hub: an ST LSM6DS3 on SPI, which the
 * modem's sensor firmware drives under stock. Only the accelerometer
 * runs, at 26 Hz in low power, for the chip's own step counter and its
 * tilt detection; the gyro stays off. A tilt is a change of more than
 * 35 degrees, which latches INT1 until FUNC_SRC is read; the line is an
 * MPM wake pin, so it wakes the watch from power collapse. A tilt with
 * the screen facing down is dropped. Screen on, the tilt interrupt is
 * off and the output registers give the accelerometer for the
 * nightstand turn. */
#include "asteroid.h"
#include "sensors/hub.h"

#define PINS		(board->hub_pins)
#define SPI_HZ		4000000
#define READ		0x80

#define WHO_AM_I	0x0f
#define WHO_AM_I_ID	0x69
#define CTRL1_XL	0x10
#define CTRL2_G		0x11
#define CTRL3_C		0x12
#define CTRL4_C		0x13
#define CTRL6_C		0x15
#define CTRL10_C	0x19
#define STATUS_REG	0x1e
#define OUTX_L_XL	0x28
#define STEP_COUNTER_L	0x4b
#define FUNC_SRC	0x53
#define TAP_CFG		0x58
#define MD1_CFG		0x5e

#define XL_26HZ_2G	0x20
#define SW_RESET	0x01
#define BDU_IF_INC	0x44
#define I2C_DISABLE	0x04
#define XL_LOW_POWER	0x10
#define FUNC_EN		0x04
#define PEDO_RST_STEP	0x02
#define PEDO_TILT_LIR	0x61
#define INT1_TILT	0x02
#define TILT_IA		0x20

#define RESET_MS	10
#define ACCEL_MS	100

static bool ready, tilt_enabled, tilted, accel_wanted;
static volatile bool pending;
static uint16_t steps_raw;
static uint32_t steps_total, tilts, dropped, irqs, errors, accel_t;

static int xfer(const uint8_t *tx, uint8_t *rx, unsigned n)
{
	gpio_set(PINS.cs, 0);
	int r = spi_xfer(PINS.spi, tx, rx, n);
	gpio_set(PINS.cs, 1);
	if (r)
		errors++;
	return r;
}

static int rd(uint8_t reg, uint8_t *v, unsigned n)
{
	uint8_t tx[8] = { (uint8_t)(reg | READ) }, rx[8];
	if (n > sizeof rx - 1)
		return -1;
	int r = xfer(tx, rx, n + 1);
	if (!r)
		for (unsigned i = 0; i < n; i++)
			v[i] = rx[i + 1];
	return r;
}

static int wr(uint8_t reg, uint8_t v)
{
	uint8_t tx[2] = { reg, v };
	return xfer(tx, NULL, 2);
}

/* Device axes from the chip's, as the vendor's sensor registry maps
 * them: minus chip X, chip Y, minus chip Z. */
static int16_t neg(int16_t v) { return (int16_t)(v == INT16_MIN ? INT16_MAX : -v); }

static bool sample(int16_t v[3])
{
	uint8_t b[6];
	if (rd(OUTX_L_XL, b, 6))
		return false;
	v[0] = neg((int16_t)(b[0] | b[1] << 8));
	v[1] = (int16_t)(b[2] | b[3] << 8);
	v[2] = neg((int16_t)(b[4] | b[5] << 8));
	return true;
}

static void pin_irq(unsigned pin)
{
	irqs++;
	gpio_irq_enable(PINS.irq, false);
	pending = true;
}

/* The vendor's sensor firmware may have left the chip running, so it
 * is reset first, and its step counter too, which the reset may not
 * clear. */
static const uint8_t setup[][2] = {
	{ CTRL4_C, I2C_DISABLE }, { CTRL3_C, BDU_IF_INC }, { CTRL2_G, 0x00 }, { CTRL6_C, XL_LOW_POWER },
	{ CTRL1_XL, XL_26HZ_2G }, { CTRL10_C, FUNC_EN }, { TAP_CFG, PEDO_TILT_LIR }, { MD1_CFG, INT1_TILT },
	{ CTRL10_C, FUNC_EN | PEDO_RST_STEP }, { CTRL10_C, FUNC_EN },
};

static bool lsm6ds3_init(void)
{
	gpio_set(PINS.cs, 1);
	gpio_config(PINS.cs, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_config(PINS.irq, 0, GPIO_INPUT, GPIO_PULL_DOWN, 2);
	const uint8_t spi_pins[3] = { PINS.gpio, PINS.gpio + 1, PINS.gpio + 3 };
	if (spi_init(PINS.spi, spi_pins, PINS.func, 0, SPI_HZ)) {
		log_printf("motion: no spi\n");
		return false;
	}
	uint8_t id = 0;
	if (rd(WHO_AM_I, &id, 1) || id != WHO_AM_I_ID) {
		log_printf("motion: lsm6ds3 not found (id %02x)\n", id);
		return false;
	}
	wr(CTRL3_C, SW_RESET);
	delay_ms(RESET_MS);
	for (unsigned i = 0; i < ARRAY_SIZE(setup); i++)
		if (wr(setup[i][0], setup[i][1])) {
			log_printf("motion: lsm6ds3 setup failed at %02x\n", setup[i][0]);
			return false;
		}
	uint8_t junk;
	rd(FUNC_SRC, &junk, 1);
	gpio_irq_config(PINS.irq, GPIO_IRQ_LEVEL_HIGH, pin_irq);
	wake_pin(PINS.mpm, true, true);
	gpio_irq_enable(PINS.irq, true);
	tilt_enabled = ready = true;
	log_printf("motion: lsm6ds3 steps and tilt on, int1 %u\n", gpio_get(PINS.irq));
	return true;
}

/* The pin's status stays set after the line has dropped again, which
 * would raise the interrupt a second time. */
static void rearm(void)
{
	if (!tilt_enabled)
		return;
	gpio_irq_ack(PINS.irq);
	gpio_irq_enable(PINS.irq, true);
}

/* A latch that cannot be read stays high, so the pin is left masked and
 * tried again next pass. */
static void lsm6ds3_poll(void)
{
	if (!ready || !pending)
		return;
	uint8_t src = 0;
	if (rd(FUNC_SRC, &src, 1))
		return;
	pending = false;
	int16_t v[3] = { 0, 0, 0 };
	if (!(src & TILT_IA) || !sample(v)) {
		rearm();
		return;
	}
	if (v[2] < 0) {
		dropped++;
	} else {
		tilts++;
		tilted = true;
	}
	log_printf("motion: tilt, %d %d %d%s\n", v[0], v[1], v[2], v[2] < 0 ? ", facing down" : "");
	rearm();
}

static bool lsm6ds3_can_sleep(void) { return !pending; }

static bool lsm6ds3_tilted(void)
{
	bool t = tilted;
	tilted = false;
	return t;
}

/* Off while the screen is on; a tilt latched before that is let go. */
static bool lsm6ds3_tilt_on(bool on)
{
	if (!ready)
		return false;
	if (on == tilt_enabled)
		return true;
	uint8_t junk;
	if (!on) {
		gpio_irq_enable(PINS.irq, false);
		wake_pin(PINS.mpm, false, true);
		if (wr(MD1_CFG, 0))
			return false;
		rd(FUNC_SRC, &junk, 1);
		pending = false;
		tilt_enabled = false;
		return true;
	}
	rd(FUNC_SRC, &junk, 1);
	if (wr(MD1_CFG, INT1_TILT))
		return false;
	tilt_enabled = true;
	gpio_irq_ack(PINS.irq);
	wake_pin(PINS.mpm, true, true);
	gpio_irq_enable(PINS.irq, true);
	return true;
}

/* The chip's counter is 16 bits; the total since boot carries on past it. */
static uint32_t lsm6ds3_steps(void)
{
	uint8_t b[2];
	if (!ready || rd(STEP_COUNTER_L, b, 2))
		return ready ? steps_total : HUB_NONE;
	uint16_t raw = (uint16_t)(b[0] | b[1] << 8);
	steps_total += (uint16_t)(raw - steps_raw);
	steps_raw = raw;
	return steps_total;
}

static void lsm6ds3_accel_on(bool on) { accel_wanted = on && ready; }

static bool lsm6ds3_accel(int16_t v[3])
{
	uint32_t now = time_ms();
	if (!accel_wanted || now - accel_t < ACCEL_MS)
		return false;
	accel_t = now;
	return sample(v);
}

static void lsm6ds3_status(void (*say)(const char *fmt, ...))
{
	int16_t v[3] = { 0, 0, 0 };
	uint8_t st = 0;
	bool got = ready && !rd(STATUS_REG, &st, 1) && sample(v);
	say("motion: lsm6ds3 %s, tilt %s, %lu tilts, %lu facing down, %lu irqs, %lu errors, %lu steps\n",
	    ready ? "ready" : "absent", tilt_enabled ? "on" : "off", tilts, dropped, irqs, errors, steps_total);
	if (got)
		say("motion: accel %d %d %d (16384 per g), status %02x, int1 %u\n", v[0], v[1], v[2], st, gpio_get(PINS.irq));
}

const struct hub_driver hub_lsm6ds3 = {
	.init = lsm6ds3_init,
	.poll = lsm6ds3_poll,
	.steps = lsm6ds3_steps,
	.tilted = lsm6ds3_tilted,
	.tilt_on = lsm6ds3_tilt_on,
	.can_sleep = lsm6ds3_can_sleep,
	.accel_on = lsm6ds3_accel_on,
	.accel = lsm6ds3_accel,
	.status = lsm6ds3_status,
};
