/* narwhal's motion sensor without a hub: a TDK ICM-40604 on SPI, which
 * the modem's sensor firmware drives under stock. It has no step counter
 * or tilt detection of its own, only wake on motion, so only the
 * accelerometer runs, at 25 Hz in low power, and the gyro stays off. A
 * change of more than WOM_MG between two samples latches INT1 until
 * INT_STATUS2 is read; the line is an MPM wake pin, so it wakes the watch
 * from power collapse. The tilt is then dory's: the screen's top edge more
 * than 0.49 g up for 300 ms within the next TILT_WINDOW_MS, with the
 * screen facing up. Screen on, the interrupt is off and the output
 * registers give the accelerometer for the nightstand turn.
 *
 * The registers are InvenSense's driver for the part in Android's sensor
 * hub; low power mode is the ICM-42600 family's, which that driver does
 * not use. */
#include "asteroid.h"
#include "sensors/hub.h"

#define PINS		(board->hub_pins)
#define SPI_HZ		4000000
#define READ		0x80

#define DEVICE_CONFIG	0x11
#define INT_CONFIG	0x14
#define ACCEL_DATA_X1	0x1f
#define INT_STATUS	0x2d
#define INTF_CONFIG0	0x4c
#define PWR_MGMT0	0x4e
#define ACCEL_CONFIG0	0x50
#define ACCEL_WOM_X_THR	0x54
#define ACCEL_WOM_Y_THR	0x55
#define ACCEL_WOM_Z_THR	0x56
#define SMD_CONFIG	0x57
#define INT_STATUS2	0x59
#define INT_CONFIG1	0x64
#define INT_SOURCE1	0x66
#define WHO_AM_I	0x75
#define WHO_AM_I_ID	0x32

#define SOFT_RESET	0x01
#define RESET_DONE	0x10
/* Sensor data big-endian as after reset, I2C off. */
#define INTF_SPI_ONLY	0x33
/* INT1 latched, push-pull, active high. */
#define INT1_LATCHED	0x07
#define INT_ASY_RST_DISABLE	0x10
#define ACCEL_LP	0x02
#define ACCEL_2G_25HZ	(3 << 5 | 10)
/* OR of the axes, each against the previous sample, wake on motion. */
#define WOM_ON		0x05
#define WOM_XYZ		0x07
#define WOM_MG		160
#define WOM_THR		(256 * WOM_MG / 1000)

#define RESET_MS	100
#define POWER_US	200
#define ACCEL_MS	100
/* The power loop wakes for a busy hub every 50 ms, but a pass on the slow
 * clock asleep can take longer, so the 300 ms are timed, over at least
 * three samples. */
#define SAMPLE_MS	50
#define TILT_WINDOW_MS	1500
#define TILT_HOLD_MS	300
#define TILT_SAMPLES	3
#define TILT_UP		8028	/* 0.49 g at 16384 per g */

static bool ready, tilt_enabled, tilted, accel_wanted, watching;
static volatile bool pending;
static unsigned up_samples;
static uint32_t watch_t, sample_t, up_t, tilts, dropped, wakes, irqs, errors, accel_t;

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

/* Device axes from the chip's, as measured on the watch: x is chip Y, y is
 * chip X, z is minus chip Z. The vendor's registry has them as is, after
 * its own driver, which we do not have. */
static bool sample(int16_t v[3])
{
	uint8_t b[6];
	if (rd(ACCEL_DATA_X1, b, 6))
		return false;
	int16_t cx = (int16_t)(b[0] << 8 | b[1]), cy = (int16_t)(b[2] << 8 | b[3]), cz = (int16_t)(b[4] << 8 | b[5]);
	v[0] = cy;
	v[1] = cx;
	v[2] = (int16_t)(cz == INT16_MIN ? INT16_MAX : -cz);
	return true;
}

static void pin_irq(unsigned pin)
{
	irqs++;
	gpio_irq_enable(PINS.irq, false);
	pending = true;
}

static const uint8_t setup[][2] = {
	{ INTF_CONFIG0, INTF_SPI_ONLY }, { INT_CONFIG, INT1_LATCHED }, { INT_CONFIG1, INT_ASY_RST_DISABLE },
	{ ACCEL_CONFIG0, ACCEL_2G_25HZ }, { ACCEL_WOM_X_THR, WOM_THR }, { ACCEL_WOM_Y_THR, WOM_THR },
	{ ACCEL_WOM_Z_THR, WOM_THR }, { PWR_MGMT0, ACCEL_LP },
};

static bool icm40604_init(void)
{
	gpio_set(PINS.cs, 1);
	gpio_config(PINS.cs, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_config(PINS.irq, 0, GPIO_INPUT, GPIO_PULL_DOWN, 2);
	const uint8_t spi_pins[3] = { PINS.gpio, PINS.gpio + 1, PINS.gpio + 3 };
	if (spi_init(PINS.spi, spi_pins, PINS.func, 3, SPI_HZ)) {
		log_printf("motion: no spi\n");
		return false;
	}
	uint8_t id = 0, st = 0;
	if (rd(WHO_AM_I, &id, 1) || id != WHO_AM_I_ID) {
		log_printf("motion: icm40604 not found (id %02x)\n", id);
		return false;
	}
	/* The vendor's sensor firmware may have left it running. */
	wr(DEVICE_CONFIG, SOFT_RESET);
	delay_ms(RESET_MS);
	if (rd(INT_STATUS, &st, 1) || !(st & RESET_DONE))
		log_printf("motion: icm40604 reset not done (%02x)\n", st);
	for (unsigned i = 0; i < ARRAY_SIZE(setup); i++)
		if (wr(setup[i][0], setup[i][1])) {
			log_printf("motion: icm40604 setup failed at %02x\n", setup[i][0]);
			return false;
		}
	delay_us(POWER_US);
	if (wr(SMD_CONFIG, WOM_ON) || wr(INT_SOURCE1, WOM_XYZ)) {
		log_printf("motion: icm40604 wake on motion not set\n");
		return false;
	}
	rd(INT_STATUS2, &st, 1);
	gpio_irq_config(PINS.irq, GPIO_IRQ_LEVEL_HIGH, pin_irq);
	wake_pin(PINS.mpm, true, true);
	gpio_irq_enable(PINS.irq, true);
	tilt_enabled = ready = true;
	log_printf("motion: icm40604 tilt on, int1 %u\n", gpio_get(PINS.irq));
	return true;
}

/* The pin's status stays set after the line has dropped again, which
 * would raise the interrupt a second time. */
static void rearm(void)
{
	uint8_t junk;
	rd(INT_STATUS2, &junk, 1);
	if (!tilt_enabled)
		return;
	gpio_irq_ack(PINS.irq);
	gpio_irq_enable(PINS.irq, true);
}

/* A motion starts a look at the next samples, with the pin masked; a
 * latch that cannot be read is tried again next pass. */
static void icm40604_poll(void)
{
	if (!ready)
		return;
	uint32_t now = time_ms();
	if (pending) {
		uint8_t st;
		if (rd(INT_STATUS2, &st, 1))
			return;
		pending = false;
		if (!tilt_enabled)
			return;
		wakes++;
		watching = true;
		watch_t = now;
		sample_t = now - SAMPLE_MS;
		up_samples = 0;
	}
	if (!watching || now - sample_t < SAMPLE_MS)
		return;
	sample_t = now;
	int16_t v[3];
	if (sample(v)) {
		bool up = v[1] >= TILT_UP;
		up_samples = up && v[2] >= 0 ? up_samples + 1 : 0;
		if (up_samples == 1)
			up_t = now;
		if (up && v[2] < 0)
			dropped++;
		if (up_samples >= TILT_SAMPLES && now - up_t >= TILT_HOLD_MS) {
			tilts++;
			tilted = true;
			log_printf("motion: tilt, %d %d %d\n", v[0], v[1], v[2]);
		}
	}
	if (tilted || now - watch_t >= TILT_WINDOW_MS) {
		watching = false;
		rearm();
	}
}

static bool icm40604_can_sleep(void) { return !pending && !watching; }

static bool icm40604_tilted(void)
{
	bool t = tilted;
	tilted = false;
	return t;
}

/* Off while the screen is on; a motion latched before that is let go. */
static bool icm40604_tilt_on(bool on)
{
	if (!ready)
		return false;
	if (on == tilt_enabled)
		return true;
	uint8_t junk;
	if (!on) {
		gpio_irq_enable(PINS.irq, false);
		wake_pin(PINS.mpm, false, true);
		if (wr(INT_SOURCE1, 0))
			return false;
		rd(INT_STATUS2, &junk, 1);
		pending = watching = false;
		tilt_enabled = false;
		return true;
	}
	rd(INT_STATUS2, &junk, 1);
	if (wr(INT_SOURCE1, WOM_XYZ))
		return false;
	tilt_enabled = true;
	gpio_irq_ack(PINS.irq);
	wake_pin(PINS.mpm, true, true);
	gpio_irq_enable(PINS.irq, true);
	return true;
}

static void icm40604_accel_on(bool on) { accel_wanted = on && ready; }

static bool icm40604_accel(int16_t v[3])
{
	uint32_t now = time_ms();
	if (!accel_wanted || now - accel_t < ACCEL_MS)
		return false;
	accel_t = now;
	return sample(v);
}

static void icm40604_status(void (*say)(const char *fmt, ...))
{
	int16_t v[3] = { 0, 0, 0 };
	uint8_t pwr = 0;
	bool got = ready && !rd(PWR_MGMT0, &pwr, 1) && sample(v);
	say("motion: icm40604 %s, tilt %s, %lu motion wakes, %lu tilts, %lu up facing down, %lu irqs, %lu errors\n",
	    ready ? "ready" : "absent", tilt_enabled ? "on" : "off", wakes, tilts, dropped, irqs, errors);
	if (got)
		say("motion: accel %d %d %d (16384 per g), pwr_mgmt0 %02x, int1 %u\n", v[0], v[1], v[2], pwr,
		    gpio_get(PINS.irq));
}

const struct hub_driver hub_icm40604 = {
	.init = icm40604_init,
	.poll = icm40604_poll,
	.tilted = icm40604_tilted,
	.tilt_on = icm40604_tilt_on,
	.can_sleep = icm40604_can_sleep,
	.accel_on = icm40604_accel_on,
	.accel = icm40604_accel,
	.status = icm40604_status,
};
