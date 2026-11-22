/* dory's motion sensors without a hub: an ST LIS3DSH accelerometer and an
 * InvenSense MPU9250 on the same I2C bus. The LIS3DSH runs the vendor's
 * wrist tilt program in its two state machines, at 25 Hz: SM1 latches
 * INT1 once the screen's top edge has been more than 0.49 g up for 300 ms,
 * SM2 hands back to SM1 once it has dropped again. A tilt with the screen
 * facing down is dropped. INT1 is level high until OUTS1 is read, and the
 * line is an MPM wake pin, so it wakes the watch from power collapse.
 * Screen on, the pins are off and the output registers give the
 * accelerometer for the nightstand turn. */
#include "asteroid.h"

#define BUS		(board->hub_pins.i2c.bus)
#define ADDR		(board->hub_pins.i2c.addr)
#define GPIO_IRQ	(board->hub_pins.irq)
#define MPM_PIN_IRQ	(board->hub_pins.mpm)
#define BUS_HZ		400000

#define WHO_AM_I	0x0f
#define WHO_AM_I_ID	0x3f
#define STAT		0x18
#define CTRL_REG4	0x20
#define CTRL_REG1	0x21
#define CTRL_REG2	0x22
#define CTRL_REG3	0x23
#define CTRL_REG5	0x24
#define CTRL_REG6	0x25
#define OUT_X_L		0x28
#define TIM2_1		0x52
#define TIM1_1		0x54
#define OUTS1		0x5f
#define OUTS2		0x7f
#define INT_OFF		0x40	/* active high, latched, both pins off */
#define INT_ON		0x58	/* the same with INT1 and INT2 on */
#define ADD_INC		0x10

#define ACCEL_MS	100

/* The vendor's setup, register and value, in its order, at 25 Hz in place
 * of its 100 Hz: the timers count samples, so its 300 ms and 2 s become
 * 8 and 50. The outputs update in whole samples, so a read never mixes
 * two. */
static const uint8_t program[][2] = {
	{ CTRL_REG1, 0x00 }, { CTRL_REG2, 0x00 }, { CTRL_REG3, INT_OFF },
	{ 0x57, 0x20 }, { 0x5a, 0x80 }, { 0x5b, 0x20 },
	{ 0x77, 0x20 }, { 0x7a, 0x80 }, { 0x7b, 0x20 },
	{ TIM1_1, 0x08 }, { TIM2_1, 0x32 },
	{ 0x40, 0x05 }, { 0x41, 0x71 }, { 0x42, 0x88 }, { 0x43, 0xff }, { 0x44, 0x11 },
	{ 0x60, 0xff }, { 0x61, 0x33 }, { 0x62, 0x07 }, { 0x63, 0x51 }, { 0x64, 0x88 }, { 0x65, 0x44 }, { 0x66, 0x11 },
	{ CTRL_REG4, 0x4f }, { CTRL_REG5, 0x00 }, { CTRL_REG6, ADD_INC }, { CTRL_REG3, INT_ON },
	{ CTRL_REG1, 0x01 }, { CTRL_REG2, 0x09 },
};

static bool ready, tilt_enabled, tilted, accel_wanted;
static volatile bool pending;
static uint32_t tilts, dropped, irqs, errors, accel_t;
static unsigned clear_fails;

static int rd(uint8_t addr, uint8_t reg, uint8_t *v, unsigned n)
{
	int r = i2c_xfer(BUS, addr, &reg, 1, v, n);
	if (r)
		errors++;
	return r;
}

static int wr(uint8_t addr, uint8_t reg, uint8_t v)
{
	uint8_t b[2] = { reg, v };
	int r = i2c_xfer(BUS, addr, b, 2, NULL, 0);
	if (r)
		errors++;
	return r;
}

/* Device axes from the chip's: x is chip Y, y is chip X, z is minus chip Z. */
static bool sample(int16_t v[3])
{
	uint8_t b[6];
	if (rd(ADDR, OUT_X_L, b, 6))
		return false;
	int16_t cx = (int16_t)(b[0] | b[1] << 8), cy = (int16_t)(b[2] | b[3] << 8), cz = (int16_t)(b[4] | b[5] << 8);
	v[0] = cy;
	v[1] = cx;
	v[2] = (int16_t)(cz == INT16_MIN ? INT16_MAX : -cz);
	return true;
}

static void pin_irq(unsigned pin)
{
	irqs++;
	gpio_irq_enable(GPIO_IRQ, false);
	pending = true;
}

/* Both machines latch their pin until their output is read; SM2's pin
 * is not wired to anything here, but is let go too. */
static bool clear_latches(void)
{
	uint8_t junk;
	bool ok = !rd(ADDR, OUTS1, &junk, 1);
	rd(ADDR, OUTS2, &junk, 1);
	return ok;
}

static bool lis3dsh_init(void)
{
	if (board->hub_pins.ldo && regulator_on(board->hub_pins.ldo, board->hub_pins.ldo_uv))
		log_printf("motion: l%u on not acknowledged\n", board->hub_pins.ldo);
	gpio_config(GPIO_IRQ, 0, GPIO_INPUT, GPIO_PULL_DOWN, 2);
	if (i2c_init(BUS, board->hub_pins.i2c.sda, board->hub_pins.i2c.scl, board->hub_pins.i2c.func, BUS_HZ)) {
		log_printf("motion: no i2c\n");
		return false;
	}
	delay_ms(10);
	uint8_t id = 0;
	if (rd(ADDR, WHO_AM_I, &id, 1) || id != WHO_AM_I_ID) {
		log_printf("motion: lis3dsh not found (id %02x)\n", id);
		return false;
	}
	for (unsigned i = 0; i < ARRAY_SIZE(program); i++)
		if (wr(ADDR, program[i][0], program[i][1])) {
			log_printf("motion: lis3dsh setup failed at %02x\n", program[i][0]);
			return false;
		}
	clear_latches();
	gpio_irq_config(GPIO_IRQ, GPIO_IRQ_LEVEL_HIGH, pin_irq);
	wake_pin(MPM_PIN_IRQ, true, true);
	gpio_irq_enable(GPIO_IRQ, true);
	tilt_enabled = ready = true;
	log_printf("motion: lis3dsh wrist tilt on, int1 %u\n", gpio_get(GPIO_IRQ));
	return true;
}

/* The pin's status stays set after the line has dropped again, which
 * would raise the interrupt a second time. */
static void rearm(void)
{
	if (!tilt_enabled)
		return;
	gpio_irq_ack(GPIO_IRQ);
	gpio_irq_enable(GPIO_IRQ, true);
}

/* A latch that cannot be read stays high, so the pin is left masked and
 * tried again next pass; after a few tries, tilt wakes are off until the
 * screen next sleeps. A tilt without a sample is not taken. */
#define CLEAR_TRIES	5

static void lis3dsh_poll(void)
{
	if (!ready || !pending)
		return;
	pending = false;
	uint8_t stat = 0;
	int16_t v[3] = { 0, 0, 0 };
	rd(ADDR, STAT, &stat, 1);
	bool got = sample(v);
	if (!clear_latches()) {
		if (++clear_fails < CLEAR_TRIES) {
			pending = true;
			return;
		}
		log_printf("motion: tilt latch not cleared, tilt wakes off\n");
		clear_fails = 0;
		wake_pin(MPM_PIN_IRQ, false, true);
		tilt_enabled = false;
		return;
	}
	clear_fails = 0;
	if (!got) {
		log_printf("motion: tilt without a sample, dropped\n");
		rearm();
		return;
	}
	if (v[2] < 0) {
		dropped++;
	} else {
		tilts++;
		tilted = true;
	}
	log_printf("motion: tilt, stat %02x, %d %d %d%s\n", stat, v[0], v[1], v[2], v[2] < 0 ? ", facing down" : "");
	rearm();
}

static bool lis3dsh_can_sleep(void) { return !pending; }

static bool lis3dsh_tilted(void)
{
	bool t = tilted;
	tilted = false;
	return t;
}

/* Off while the screen is on; a tilt latched before that is let go. */
static bool lis3dsh_tilt_on(bool on)
{
	if (!ready)
		return false;
	if (on == tilt_enabled)
		return true;
	if (!on) {
		gpio_irq_enable(GPIO_IRQ, false);
		wake_pin(MPM_PIN_IRQ, false, true);
		if (wr(ADDR, CTRL_REG3, INT_OFF))
			return false;
		clear_latches();
		pending = false;
		tilt_enabled = false;
		return true;
	}
	clear_latches();
	if (wr(ADDR, CTRL_REG3, INT_ON))
		return false;
	tilt_enabled = true;
	gpio_irq_ack(GPIO_IRQ);
	wake_pin(MPM_PIN_IRQ, true, true);
	gpio_irq_enable(GPIO_IRQ, true);
	return true;
}

static void lis3dsh_accel_on(bool on) { accel_wanted = on && ready; }

static bool lis3dsh_accel(int16_t v[3])
{
	uint32_t now = time_ms();
	if (!accel_wanted || now - accel_t < ACCEL_MS)
		return false;
	accel_t = now;
	return sample(v);
}

static void lis3dsh_status(void (*say)(const char *fmt, ...))
{
	int16_t v[3] = { 0, 0, 0 };
	bool got = ready && sample(v);
	say("motion: lis3dsh %s, tilt %s, %lu tilts, %lu facing down, %lu irqs, %lu errors\n",
	    ready ? "ready" : "absent", tilt_enabled ? "on" : "off", tilts, dropped, irqs, errors);
	if (got)
		say("motion: accel %d %d %d (16384 per g), int1 %u\n", v[0], v[1], v[2], gpio_get(GPIO_IRQ));
}

const struct hub_driver hub_lis3dsh = {
	.init = lis3dsh_init,
	.poll = lis3dsh_poll,
	.tilted = lis3dsh_tilted,
	.tilt_on = lis3dsh_tilt_on,
	.can_sleep = lis3dsh_can_sleep,
	.accel_on = lis3dsh_accel_on,
	.accel = lis3dsh_accel,
	.status = lis3dsh_status,
};
