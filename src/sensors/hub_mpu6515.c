/* lenok's motion sensors without a hub: an InvenSense MPU-6515 and the
 * AK8963 magnetometer behind its I2C bypass, which the vendor ran from
 * its ADSP. The chip comes out of reset with everything running, so it
 * sleeps until something wants it. With the screen off, wrist tilt is
 * the chip's wake-on-motion in its low power cycle: a jolt raises INT,
 * an MPM wake pin, and the accelerometer then runs at 25 Hz into the
 * FIFO for STREAM_MS, drained by the main loop into the wrist raise
 * detector, longer while the jolts go on. INT is latched until
 * INT_STATUS is read. The compass runs the accelerometer and gyroscope
 * at 50 Hz and the AK8963 at 100 Hz, sampled by the main loop. The
 * PAH8001 heart rate sensor on the same bus is run from here too. */
#include "asteroid.h"
#include "sensors/hub.h"
#include "sensors/raise.h"
#include "sensors/compass.h"
#include "sensors/fusion.h"
#include "sensors/pah8001.h"

#define BUS		(board->hub_pins.i2c.bus)
#define ADDR		(board->hub_pins.i2c.addr)
#define GPIO_IRQ	(board->hub_pins.irq)
#define MPM_PIN_IRQ	(board->hub_pins.mpm)
#define BUS_HZ		400000

#define SMPLRT_DIV	0x19
#define CONFIG		0x1a
#define GYRO_CONFIG	0x1b
#define ACCEL_CONFIG	0x1c
#define ACCEL_CONFIG2	0x1d
#define LP_ACCEL_ODR	0x1e
#define WOM_THR		0x1f
#define FIFO_EN		0x23
#define INT_PIN_CFG	0x37
#define INT_ENABLE	0x38
#define INT_STATUS	0x3a
#define ACCEL_XOUT_H	0x3b
#define GYRO_XOUT_H	0x43
#define MOT_DETECT_CTRL	0x69
#define USER_CTRL	0x6a
#define PWR_MGMT_1	0x6b
#define PWR_MGMT_2	0x6c
#define FIFO_COUNTH	0x72
#define FIFO_R_W	0x74
#define WHO_AM_I	0x75

#define ID_MPU6515	0x74
#define H_RESET		0x80
#define SLEEP		0x40
#define CYCLE		0x20
#define CLK_PLL		0x01
#define GYRO_OFF	0x07
/* Active high, latched until any read, bypass to the AK8963 on. */
#define PIN_CFG		0x32
#define INT_WOM		0x40
#define WOM_COMPARE	0xc0
#define FIFO_ACCEL	0x08
#define USER_FIFO_EN	0x40
#define USER_FIFO_RST	0x04
#define ACCEL_8G	0x10
#define GYRO_2000DPS	0x18
#define DLPF_41HZ	0x03
#define DLPF_20HZ	0x04
/* For wake-on-motion: the 1 kHz path, 184 Hz bandwidth. */
#define ACCEL_LP_DLPF	0x09
/* 15.63 Hz in the low power cycle. */
#define LP_ODR_16HZ	0x06
/* In 4 mg. */
#define WOM_LSB		20
#define DIV_25HZ	39
#define DIV_50HZ	19
#define MG_PER_8G	(1000.0f / 4096)
#define GYRO_TO_RADS	(3.14159265f / 180 / 16.4f)

#define AK_ADDR		0x0f
#define AK_WIA		0x00
#define AK_ST1		0x02
#define AK_CNTL1	0x0a
#define AK_ASAX		0x10
#define AK_ID		0x48
#define AK_POWER_DOWN	0x00
#define AK_FUSE_ROM	0x0f
#define AK_16BIT_100HZ	0x16
#define AK_ST2_HOFL	0x08
#define AK_UT_PER_COUNT	0.15f

#define STREAM_MS	2500
#define DRAIN_MAX	32
#define COMPASS_MS	20
#define FIELD_STALE_MS	500
#define ACCEL_MS	40

enum mode { M_OFF, M_WOM, M_STREAM, M_ACCEL, M_COMPASS, M_UNSURE };

static bool ready, ak_ready, tilt_wanted, accel_wanted, compass_wanted, tilt, armed;
static volatile bool pending;
static enum mode mode = M_UNSURE;
static struct raise raise;
static struct compass compass;
static struct fusion fusion;
static float asa[3] = { 1, 1, 1 }, field[3];
static int field_accuracy = -1;
static int16_t latest[3];
static bool have_latest, restarted;
static uint8_t was;
static uint32_t stream_until, fed_t, compass_due, compass_t, field_t, accel_t;
static uint32_t irqs, jolts, samples, raises, errors;

static int rd(uint8_t addr, uint8_t reg, uint8_t *v, unsigned n)
{
	int r = i2c_xfer(BUS, addr, &reg, 1, v, n);
	errors += r != 0;
	return r;
}

static int wr(uint8_t addr, uint8_t reg, uint8_t v)
{
	uint8_t b[2] = { reg, v };
	int r = i2c_xfer(BUS, addr, b, 2, NULL, 0);
	errors += r != 0;
	return r;
}

static int16_t be16(const uint8_t *p) { return (int16_t)(p[0] << 8 | p[1]); }

/* The watch's axes from the chip's, as the vendor's registry maps them:
 * x is chip y, y is minus chip x. */
static void axes(const uint8_t *raw, int16_t v[3])
{
	int16_t cx = be16(raw), cy = be16(raw + 2), cz = be16(raw + 4);
	v[0] = (int16_t)(cy * MG_PER_8G);
	v[1] = (int16_t)(-cx * MG_PER_8G);
	v[2] = (int16_t)(cz * MG_PER_8G);
}

static void pin_irq(unsigned pin)
{
	irqs++;
	gpio_irq_enable(GPIO_IRQ, false);
	pending = true;
}

/* The pin's status stays set after the line has dropped again, which
 * would raise the interrupt a second time. */
static void irq_on(bool on)
{
	if (on == armed)
		return;
	armed = on;
	if (on) {
		gpio_irq_ack(GPIO_IRQ);
		wake_pin(MPM_PIN_IRQ, true, true);
		gpio_irq_enable(GPIO_IRQ, true);
	} else {
		gpio_irq_enable(GPIO_IRQ, false);
		wake_pin(MPM_PIN_IRQ, false, true);
	}
}

static int ak_mode(uint8_t m)
{
	int r = wr(AK_ADDR, AK_CNTL1, AK_POWER_DOWN);
	delay_us(100);
	return r || m == AK_POWER_DOWN ? r : wr(AK_ADDR, AK_CNTL1, m);
}

/* The registers for a mode, written whole each time, so a write that
 * failed is mended by the next change. */
static bool set_mode(enum mode m)
{
	if (m == mode)
		return true;
	uint8_t a = ADDR;
	int e = 0;
	if (mode == M_COMPASS && ak_ready)
		e |= ak_mode(AK_POWER_DOWN);
	switch (m) {
	case M_OFF:
		e |= wr(a, INT_ENABLE, 0);
		e |= wr(a, PWR_MGMT_1, SLEEP);
		break;
	case M_WOM:
		e |= wr(a, PWR_MGMT_1, 0);
		e |= wr(a, PWR_MGMT_2, GYRO_OFF);
		e |= wr(a, USER_CTRL, 0);
		e |= wr(a, ACCEL_CONFIG, ACCEL_8G);
		e |= wr(a, ACCEL_CONFIG2, ACCEL_LP_DLPF);
		e |= wr(a, MOT_DETECT_CTRL, WOM_COMPARE);
		e |= wr(a, WOM_THR, WOM_LSB);
		e |= wr(a, LP_ACCEL_ODR, LP_ODR_16HZ);
		e |= wr(a, INT_ENABLE, INT_WOM);
		e |= wr(a, PWR_MGMT_1, CYCLE);
		break;
	case M_STREAM:
	case M_ACCEL:
		e |= wr(a, PWR_MGMT_1, CLK_PLL);
		e |= wr(a, PWR_MGMT_2, GYRO_OFF);
		e |= wr(a, CONFIG, DLPF_20HZ);
		e |= wr(a, SMPLRT_DIV, DIV_25HZ);
		e |= wr(a, ACCEL_CONFIG, ACCEL_8G);
		e |= wr(a, ACCEL_CONFIG2, DLPF_20HZ);
		e |= wr(a, MOT_DETECT_CTRL, WOM_COMPARE);
		e |= wr(a, WOM_THR, WOM_LSB);
		if (m == M_STREAM) {
			e |= wr(a, FIFO_EN, FIFO_ACCEL);
			e |= wr(a, USER_CTRL, USER_FIFO_RST);
			e |= wr(a, USER_CTRL, USER_FIFO_EN);
			e |= wr(a, INT_ENABLE, INT_WOM);
		} else {
			e |= wr(a, FIFO_EN, 0);
			e |= wr(a, USER_CTRL, 0);
			e |= wr(a, INT_ENABLE, 0);
		}
		break;
	case M_COMPASS:
		e |= wr(a, PWR_MGMT_1, CLK_PLL);
		e |= wr(a, PWR_MGMT_2, 0);
		e |= wr(a, CONFIG, DLPF_41HZ);
		e |= wr(a, SMPLRT_DIV, DIV_50HZ);
		e |= wr(a, GYRO_CONFIG, GYRO_2000DPS);
		e |= wr(a, ACCEL_CONFIG, ACCEL_8G);
		e |= wr(a, ACCEL_CONFIG2, DLPF_41HZ);
		e |= wr(a, FIFO_EN, 0);
		e |= wr(a, USER_CTRL, 0);
		e |= wr(a, INT_ENABLE, 0);
		if (ak_ready)
			e |= ak_mode(AK_16BIT_100HZ);
		break;
	case M_UNSURE:
		break;
	}
	uint8_t junk;
	rd(a, INT_STATUS, &junk, 1);
	mode = e ? M_UNSURE : m;
	if (e)
		log_printf("motion: mode %u not taken\n", m);
	return !e;
}

/* Screen off, tilt runs on wake-on-motion between bursts; screen on, the
 * accelerometer or the compass. */
static void apply(uint32_t now)
{
	enum mode m = compass_wanted ? M_COMPASS
		      : tilt_wanted && (int32_t)(stream_until - now) > 0 ? M_STREAM
		      : accel_wanted ? M_ACCEL
		      : tilt_wanted ? M_WOM
		      : M_OFF;
	if (m == M_STREAM && mode != M_STREAM)
		restarted = true;
	if (m != M_STREAM && m != M_ACCEL && m != M_COMPASS)
		have_latest = false;
	set_mode(m);
	irq_on(mode == M_WOM || mode == M_STREAM);
}

static bool ak_probe(void)
{
	uint8_t id = 0, fuse[3];
	if (rd(AK_ADDR, AK_WIA, &id, 1) || id != AK_ID) {
		log_printf("motion: no ak8963 (id %02x)\n", id);
		return false;
	}
	if (ak_mode(AK_FUSE_ROM) || rd(AK_ADDR, AK_ASAX, fuse, 3) || ak_mode(AK_POWER_DOWN))
		return false;
	for (int i = 0; i < 3; i++)
		asa[i] = ((float)fuse[i] - 128) / 256 + 1;
	log_printf("motion: ak8963, asa %02x %02x %02x\n", fuse[0], fuse[1], fuse[2]);
	return true;
}

static bool mpu_init(void)
{
	if (board->hub_pins.ldo && regulator_on(board->hub_pins.ldo, board->hub_pins.ldo_uv))
		log_printf("motion: l%u on not acknowledged\n", board->hub_pins.ldo);
	gpio_config(GPIO_IRQ, 0, GPIO_INPUT, GPIO_PULL_DOWN, 2);
	if (i2c_init(BUS, board->hub_pins.i2c.sda, board->hub_pins.i2c.scl, board->hub_pins.i2c.func, BUS_HZ)) {
		log_printf("motion: no i2c\n");
		return false;
	}
	uint8_t id = 0;
	rd(ADDR, PWR_MGMT_1, &was, 1);
	if (rd(ADDR, WHO_AM_I, &id, 1) || id != ID_MPU6515) {
		log_printf("motion: no mpu6515 (id %02x)\n", id);
		return false;
	}
	wr(ADDR, PWR_MGMT_1, H_RESET);
	delay_ms(100);
	wdt_feed();
	if (wr(ADDR, INT_PIN_CFG, PIN_CFG)) {
		log_printf("motion: mpu6515 setup failed\n");
		return false;
	}
	ak_ready = ak_probe();
	pah8001_init();
	raise_init(&raise);
	gpio_irq_config(GPIO_IRQ, GPIO_IRQ_LEVEL_HIGH, pin_irq);
	ready = true;
	set_mode(M_OFF);
	log_printf("motion: mpu6515, pwr_mgmt_1 was %02x, %s\n", was, mode == M_OFF ? "asleep" : "not asleep");
	return true;
}

/* The first sample after a gap stands for the time it lasted, as the
 * raise detector wants a steady rate. */
static void feed(const int16_t v[3], uint32_t now)
{
	float a[3];
	for (int i = 0; i < 3; i++)
		a[i] = v[i] * (9.8f / 1000);
	unsigned n = restarted && samples ? raise_repeats((int32_t)(now - fed_t)) : 1;
	restarted = false;
	fed_t = now;
	samples++;
	while (n--)
		if (raise_feed(&raise, a, false) == RAISE_UP) {
			raises++;
			tilt = true;
		}
}

static void drain(uint32_t now)
{
	uint8_t c[2], s[6];
	if (rd(ADDR, FIFO_COUNTH, c, 2))
		return;
	unsigned n = (unsigned)((c[0] & 0x1f) << 8 | c[1]) / 6;
	for (unsigned k = 0; k < n && k < DRAIN_MAX && !rd(ADDR, FIFO_R_W, s, 6); k++) {
		axes(s, latest);
		have_latest = true;
		feed(latest, now);
	}
}

static void sample_compass(uint32_t now)
{
	if ((int32_t)(now - compass_due) < 0)
		return;
	compass_due = now + COMPASS_MS;
	uint8_t m[8], ag[14];
	int16_t a[3];
	if (rd(ADDR, ACCEL_XOUT_H, ag, sizeof ag))
		return;
	axes(ag, a);
	for (int i = 0; i < 3; i++)
		latest[i] = a[i];
	have_latest = true;
	int16_t gx = be16(ag + 8), gy = be16(ag + 10), gz = be16(ag + 12);
	float rate[3] = { gy * GYRO_TO_RADS, -gx * GYRO_TO_RADS, gz * GYRO_TO_RADS };
	float dt = (float)(now - compass_t) / 1000;
	compass_t = now;
	bool fresh = false;
	if (ak_ready && !rd(AK_ADDR, AK_ST1, m, sizeof m) && (m[0] & 1)) {
		int16_t hx = (int16_t)(m[1] | m[2] << 8), hy = (int16_t)(m[3] | m[4] << 8), hz = (int16_t)(m[5] | m[6] << 8);
		if (m[7] & AK_ST2_HOFL) {
			field_accuracy = compass_overflow(&compass);
		} else {
			/* The vendor's map for the magnetometer: x is minus chip y, y is chip x. */
			float raw[3] = { -hy * asa[1] * AK_UT_PER_COUNT, hx * asa[0] * AK_UT_PER_COUNT,
					 hz * asa[2] * AK_UT_PER_COUNT };
			field_accuracy = compass_feed(&compass, raw, rate, (float)(now - field_t) / 1000, field);
			field_t = now;
			fresh = true;
		}
	}
	float af[3] = { a[0], a[1], a[2] };
	fusion_update(&fusion, af, rate, fresh ? field : NULL, fresh ? field_accuracy : 0, dt);
}

/* A jolt keeps the burst going; INT_STATUS lets the latch go. */
static void mpu_poll(void)
{
	if (!ready)
		return;
	pah8001_poll();
	uint32_t now = time_ms();
	if (pending) {
		pending = false;
		uint8_t st = 0;
		if (!rd(ADDR, INT_STATUS, &st, 1) && (st & INT_WOM) && tilt_wanted) {
			jolts++;
			stream_until = now + STREAM_MS;
		}
		if (armed) {
			gpio_irq_ack(GPIO_IRQ);
			gpio_irq_enable(GPIO_IRQ, true);
		}
	}
	apply(now);
	if (mode == M_STREAM)
		drain(now);
	else if (mode == M_COMPASS)
		sample_compass(now);
}

static bool mpu_can_sleep(void) { return !pending && mode != M_STREAM && pah8001_can_sleep(); }

static bool mpu_tilted(void)
{
	bool t = tilt;
	tilt = false;
	return t;
}

static bool mpu_tilt_on(bool on)
{
	if (!ready)
		return false;
	tilt_wanted = on;
	if (!on)
		stream_until = time_ms();
	apply(time_ms());
	return mode != M_UNSURE;
}

static void mpu_accel_on(bool on)
{
	accel_wanted = on && ready;
	if (ready)
		apply(time_ms());
}

static bool mpu_accel(int16_t v[3])
{
	uint8_t raw[6];
	uint32_t now = time_ms();
	if (!ready || !accel_wanted)
		return false;
	if (have_latest && (mode == M_STREAM || mode == M_COMPASS)) {
		for (int i = 0; i < 3; i++)
			v[i] = latest[i];
		return true;
	}
	if (now - accel_t < ACCEL_MS || rd(ADDR, ACCEL_XOUT_H, raw, 6))
		return false;
	accel_t = now;
	axes(raw, v);
	return true;
}

static void mpu_compass_on(bool on)
{
	if (!ready || on == compass_wanted)
		return;
	compass_wanted = on;
	if (on) {
		compass_init(&compass);
		fusion_init(&fusion);
		field_accuracy = -1;
		compass_due = compass_t = field_t = time_ms();
	}
	apply(time_ms());
}

static int mpu_heading(float *degrees)
{
	float out[3];
	if (!compass_wanted || field_accuracy < 0 || time_ms() - field_t > FIELD_STALE_MS || !fusion_angles(&fusion, out))
		return -1;
	*degrees = out[0];
	return field_accuracy;
}

static void mpu_status(void (*say)(const char *fmt, ...))
{
	static const char *const names[] = { "off", "wake-on-motion", "streaming", "accelerometer", "compass", "unsure" };
	say("motion: mpu6515 %s, %s, ak8963 %s, tilt %s; %lu irqs, %lu jolts, %lu samples, %lu raises, %lu errors\n",
	    ready ? "ready" : "absent", names[mode], ak_ready ? "ready" : "absent", tilt_wanted ? "on" : "off", irqs, jolts,
	    samples, raises, errors);
	if (have_latest)
		say("motion: accel %d %d %d mg, int %u\n", latest[0], latest[1], latest[2], gpio_get(GPIO_IRQ));
	pah8001_status(say);
}

const struct hub_driver hub_mpu6515 = {
	.init = mpu_init,
	.poll = mpu_poll,
	.tilted = mpu_tilted,
	.tilt_on = mpu_tilt_on,
	.can_sleep = mpu_can_sleep,
	.accel_on = mpu_accel_on,
	.accel = mpu_accel,
	.compass_on = mpu_compass_on,
	.heading = mpu_heading,
	.status = mpu_status,
	.heart_rate_on = pah8001_on,
	.heart_rate = pah8001_rate,
};
