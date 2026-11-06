/* smelt's M4 sensor hub, an STM32 on I2C. It runs the firmware already in
 * its flash, which the vendor's software put there; nothing here erases
 * or writes the flash, and no command that could is sent. As the vendor's
 * driver does, it is reset into the ROM bootloader with boot0 high, the
 * mark at the end of the firmware is checked and the bootloader told to
 * jump to it. The firmware takes 2.5 s to come up, so init returns and
 * the poll finishes the job: the pedometer, and the wake line for a
 * gesture, which reading the status clears. */
#include "asteroid.h"
#include "sensors/hub.h"
#include "sensors/m4_proto.h"

#define QUP		(board->hub_pins.i2c.qup)
#define ADDR		(board->hub_pins.i2c.addr)
#define GPIO_WAKE	(board->hub_pins.irq)
#define GPIO_RESET	(board->hub_pins.reset)
#define GPIO_BOOT0	(board->hub_pins.boot0)
#define MPM_PIN_WAKE	(board->hub_pins.mpm)
#define BUS_HZ		384000

#define BOOT_MS		2500
#define BL_READY_MS	600
#define BL_ATTEMPTS	60
#define STEPS_MS	10000

static enum { HUB_OFF, HUB_BOOTING, HUB_READY } state;
static uint32_t go_t, steps_t, steps;
static uint16_t version;
static volatile bool wake_pending;
static bool tilt;

static void wait_ms(unsigned ms)
{
	for (; ms >= 10; ms -= 10) {
		delay_ms(10);
		wdt_feed();
	}
}

/* An answer to a bootloader command; it may ask for time. */
static bool bl_ack(void)
{
	for (int i = 0; i <= BL_ATTEMPTS; i++) {
		uint8_t a = 0;
		if (i2c_qup_xfer(QUP, ADDR, NULL, 0, &a, 1))
			return false;
		if (a == M4_BL_ACK)
			return true;
		if (a != M4_BL_BUSY)
			return false;
		wait_ms(100);
	}
	return false;
}

static bool bl_opcode(uint8_t op)
{
	uint8_t b[2] = { op, (uint8_t)~op };
	return !i2c_qup_xfer(QUP, ADDR, b, 2, NULL, 0) && bl_ack();
}

static bool bl_address(uint32_t addr)
{
	uint8_t b[5];
	m4_bl_address(addr, b);
	return !i2c_qup_xfer(QUP, ADDR, b, 5, NULL, 0) && bl_ack();
}

static bool bl_read(uint32_t addr, uint8_t *data, unsigned n)
{
	uint8_t len[2] = { (uint8_t)(n - 1), (uint8_t)((n - 1) ^ 0xff) };
	return bl_opcode(M4_BL_READ) && bl_address(addr) && !i2c_qup_xfer(QUP, ADDR, len, 2, NULL, 0) && bl_ack() &&
	       !i2c_qup_xfer(QUP, ADDR, NULL, 0, data, n);
}

static int reg_read(uint8_t bank, uint8_t offset, uint8_t *data, unsigned n)
{
	uint8_t a[2] = { bank, offset };
	return i2c_qup_xfer(QUP, ADDR, a, 2, data, n);
}

static int reg_write(uint8_t bank, uint8_t offset, const uint8_t *data, unsigned n)
{
	uint8_t b[6] = { bank, offset };
	for (unsigned i = 0; i < n && i < 4; i++)
		b[2 + i] = data[i];
	return i2c_qup_xfer(QUP, ADDR, b, 2 + n, NULL, 0);
}

static void wake_line(unsigned pin)
{
	tlmm_irq_enable(GPIO_WAKE, false);
	tlmm_irq_ack(GPIO_WAKE);
	wake_pending = true;
}

/* boot0 high through the reset selects the ROM bootloader, then the reset
 * line is let go. */
static bool m4_init(void)
{
	uint8_t barker[M4_BARKER_SIZE];
	tlmm_config(GPIO_WAKE, 0, TLMM_INPUT, TLMM_PULL_NONE, 2);
	if (i2c_qup_init(QUP, board->hub_pins.i2c.sda, board->hub_pins.i2c.scl, board->hub_pins.i2c.func, BUS_HZ))
		return false;
	tlmm_set(GPIO_BOOT0, 1);
	tlmm_config(GPIO_BOOT0, 0, TLMM_OUTPUT, TLMM_PULL_NONE, 2);
	delay_ms(5);
	tlmm_set(GPIO_RESET, 0);
	tlmm_config(GPIO_RESET, 0, TLMM_OUTPUT, TLMM_PULL_NONE, 2);
	delay_ms(10);
	tlmm_config(GPIO_RESET, 0, TLMM_INPUT, TLMM_PULL_NONE, 2);
	wait_ms(BL_READY_MS);
	if (!bl_read(M4_BARKER_ADDR, barker, sizeof barker)) {
		log_printf("hub: m4 bootloader not answering\n");
		return false;
	}
	if (!m4_barker_ok(barker)) {
		log_printf("hub: no firmware mark in the m4's flash, not starting it\n");
		return false;
	}
	if (!bl_opcode(M4_BL_GO) || !bl_address(M4_FLASH_BASE)) {
		log_printf("hub: m4 bootloader refused to start the firmware\n");
		return false;
	}
	state = HUB_BOOTING;
	go_t = time_ms();
	return true;
}

static void set_time(void)
{
	uint32_t t = pmic_rtc_read();
	uint8_t b[4] = { (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16), (uint8_t)(t >> 24) };
	if (state == HUB_READY)
		reg_write(M4_BANK_GENERAL, M4_GENERAL_UTC, b, 4);
}

static void started(void)
{
	uint8_t v[2] = { 0, 0 }, on = 1, enable[4];
	if (reg_read(M4_BANK_GENERAL, M4_GENERAL_VERSION, v, 2)) {
		log_printf("hub: m4 firmware not answering\n");
		state = HUB_OFF;
		return;
	}
	version = (uint16_t)(v[0] | v[1] << 8);
	state = HUB_READY;
	set_time();
	reg_write(M4_BANK_PEDOMETER, M4_PEDOMETER_ENABLE, &on, 1);
	reg_write(M4_BANK_METS, M4_METS_ENABLE, &on, 1);
	if (!reg_read(M4_BANK_GENERAL, M4_GENERAL_WAKE_ENABLE, enable, 4)) {
		enable[0] |= M4_WAKE_GESTURE;
		reg_write(M4_BANK_GENERAL, M4_GENERAL_WAKE_ENABLE, enable, 4);
	}
	tlmm_irq_config(GPIO_WAKE, TLMM_IRQ_LEVEL_HIGH, wake_line);
	tlmm_irq_enable(GPIO_WAKE, true);
	log_printf("hub: m4 firmware %04x up\n", version);
}

static void m4_poll(void)
{
	i2c_qup_poll(QUP);
	if (state == HUB_BOOTING && time_ms() - go_t >= BOOT_MS)
		started();
	if (!wake_pending)
		return;
	wake_pending = false;
	uint8_t s[4], g[3] = { 0 };
	if (!reg_read(M4_BANK_GENERAL, M4_GENERAL_WAKE_STATUS, s, 4)) {
		uint32_t status = m4_u32(s);
		if (status & M4_WAKE_GESTURE) {
			/* One register at a time, as the vendor reads them. */
			int r = 0;
			for (unsigned i = 0; i < 3; i++)
				r |= reg_read(M4_BANK_GESTURE, (uint8_t)(M4_GESTURE_FIRST + i), &g[i], 1);
			if (!r && m4_is_tilt(status, g[0], (int8_t)g[2]))
				tilt = true;
		}
	}
	tlmm_irq_enable(GPIO_WAKE, true);
}

static bool m4_tilt_on(bool on)
{
	uint8_t enable[4];
	if (state != HUB_READY || reg_read(M4_BANK_GENERAL, M4_GENERAL_WAKE_ENABLE, enable, 4))
		return false;
	enable[0] = (uint8_t)(on ? enable[0] | M4_WAKE_GESTURE : enable[0] & ~M4_WAKE_GESTURE);
	return !reg_write(M4_BANK_GENERAL, M4_GENERAL_WAKE_ENABLE, enable, 4);
}

/* The rate is the time between samples in ms, -1 off. */
#define ACCEL_MS	200

static bool accel_running;
static uint32_t accel_t;

static void m4_accel_on(bool on)
{
	int16_t rate = on ? ACCEL_MS : -1;
	uint8_t b[2] = { (uint8_t)rate, (uint8_t)((uint16_t)rate >> 8) };
	if (on == accel_running || state != HUB_READY)
		return;
	if (!reg_write(M4_BANK_ACCEL, M4_ACCEL_RATE, b, 2))
		accel_running = on;
	log_printf("hub: accelerometer %s\n", on ? "on" : "off");
}

static bool m4_accel(int16_t v[3])
{
	uint32_t now = time_ms();
	if (!accel_running || now - accel_t < ACCEL_MS)
		return false;
	accel_t = now;
	for (unsigned i = 0; i < 3; i++) {
		uint8_t b[4];
		if (reg_read(M4_BANK_ACCEL, (uint8_t)(M4_ACCEL_X + 4 * i), b, 4))
			return false;
		/* 65536 to 1 g, down to the 2048 the other hubs give. */
		int32_t a = (int32_t)m4_u32(b) / 32;
		v[i] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
	}
	return true;
}

static uint32_t m4_steps(void)
{
	if (state != HUB_READY)
		return HUB_NONE;
	uint32_t now = time_ms();
	uint8_t b[4];
	if (now - steps_t >= STEPS_MS) {
		steps_t = now;
		if (!reg_read(M4_BANK_PEDOMETER, M4_PEDOMETER_STEPS, b, 4))
			steps = m4_u32(b);
	}
	return steps;
}

static bool m4_tilted(void)
{
	bool t = tilt;
	tilt = false;
	return t;
}

/* Not until the firmware is up, or the wait for it would stretch out. */
static bool m4_can_sleep(void)
{
	return state != HUB_BOOTING;
}

static void m4_sleep(bool asleep)
{
	if (state == HUB_READY)
		spm_wake_pin(MPM_PIN_WAKE, asleep, true);
}

static void m4_status(void (*say)(const char *fmt, ...))
{
	say("hub: m4 firmware %04x, %s, %lu steps\n", version, state == HUB_READY ? "up" : "not up", steps);
}

const struct hub_driver hub_m4 = {
	.init = m4_init,
	.poll = m4_poll,
	.set_time = set_time,
	.steps = m4_steps,
	.tilted = m4_tilted,
	.tilt_on = m4_tilt_on,
	.accel_on = m4_accel_on,
	.accel = m4_accel,
	.can_sleep = m4_can_sleep,
	.sleep = m4_sleep,
	.status = m4_status,
};
