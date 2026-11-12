/* smelt's M4 sensor hub, an STM32 on I2C. It runs the firmware already in
 * its flash, which the vendor's software put there; nothing here erases
 * or writes the flash, and no command that could is sent. As the vendor's
 * driver does, it is reset into the ROM bootloader with boot0 high, the
 * mark at the end of the firmware is checked and the bootloader told to
 * jump to it. The firmware takes 2.5 s to come up, so init returns and
 * the poll finishes the job: the vendor's starting registers, the
 * pedometer, and the wake line for a gesture, which reading the status
 * clears. The hub serves a register inside its interrupt handler and may
 * hold the clock meanwhile; a transfer it is cut off from makes it crash
 * a few seconds later, so the bus gets the vendor's second of patience.
 * A crashed hub raises the wake line with nothing in the status and
 * answers the panic probe with a magic word; it is then started again,
 * as is a hub that keeps failing on the bus. Its step count starts over
 * each time, so what it had counted is carried across, as are the
 * distance and calories it works out from the wearer's profile, which it
 * forgets too and is given again after each start. The hub crashes
 * on its own about a second after it finds the watch still (a minute
 * without motion) unless the accelerometer is enabled, which also keeps
 * it from going still at all, so the accelerometer stays enabled and is
 * only read when wanted. */
#include "asteroid.h"
#include "sensors/hub.h"
#include "sensors/m4_proto.h"

#define BUS		(board->hub_pins.i2c.bus)
#define ADDR		(board->hub_pins.i2c.addr)
#define GPIO_WAKE	(board->hub_pins.irq)
#define GPIO_RESET	(board->hub_pins.reset)
#define GPIO_BOOT0	(board->hub_pins.boot0)
#define MPM_PIN_WAKE	(board->hub_pins.mpm)
#define V		(board->hub_variant)
#define BUS_HZ		384000
/* How long the hub may hold the clock: the vendor kernel's second. */
#define STRETCH_MS	1000

#define BOOT_MS		2500
#define BL_READY_MS	600
#define BL_ATTEMPTS	60
/* Reset pulses before the bootloader is given up, as the vendor retries. */
#define BL_ROUNDS	3
#define VERSION_TRIES	3
/* Reads of the step count: the hub crashes after ten in a row that give
 * the same count while it believes the watch is moving, which the block
 * read avoids; keeping them rare is the second line. */
#define STEPS_MS	30000
#define FAIL_LIMIT	5
#define RESTART_MS	30000
/* A failed read or write is tried again after this long. */
#define RETRY_MS	1000
/* Tries of one transfer, a moment apart, before it counts as failed. */
#define XFER_TRIES	3
#define XFER_RETRY_US	50

static enum { HUB_OFF, HUB_BOOTING, HUB_READY } state;
static uint32_t go_t, steps_t, steps, steps_base, restart_t, wake_retry_t;
static uint32_t activity[2], activity_base[2];
static bool activity_read, profile_set;
static struct hub_profile profile = { 30, 170, 70, false };
static uint16_t version;
static volatile bool wake_pending;
static bool tilt, tilt_wanted = true, tilt_set, wake_retry, accel_wanted, accel_set;
static unsigned fails;
static bool light_on;
static uint32_t wakes, last_wake, crashes, restarts;

static void wait_ms(unsigned ms)
{
	for (; ms >= 10; ms -= 10) {
		delay_ms(10);
		wdt_feed();
	}
}

static int xfer(const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	int ret = 0;
	for (unsigned i = 0; i < XFER_TRIES; i++) {
		ret = i2c_xfer(BUS, ADDR, w, wn, r, rn);
		if (!ret)
			break;
		delay_us(XFER_RETRY_US);
	}
	return ret;
}

/* An answer to a bootloader command; it may ask for time. */
static bool bl_ack(void)
{
	for (int i = 0; i <= BL_ATTEMPTS; i++) {
		uint8_t a = 0;
		if (xfer(NULL, 0, &a, 1))
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
	return !xfer(b, 2, NULL, 0) && bl_ack();
}

static bool bl_address(uint32_t addr)
{
	uint8_t b[5];
	m4_bl_address(addr, b);
	return !xfer(b, 5, NULL, 0) && bl_ack();
}

static bool bl_read(uint32_t addr, uint8_t *data, unsigned n)
{
	uint8_t len[2] = { (uint8_t)(n - 1), (uint8_t)((n - 1) ^ 0xff) };
	return bl_opcode(M4_BL_READ) && bl_address(addr) && !xfer(len, 2, NULL, 0) && bl_ack() &&
	       !xfer(NULL, 0, data, n);
}

static int counted(int r)
{
	fails = r ? fails + 1 : 0;
	return r;
}

static int reg_read(uint8_t bank, uint8_t offset, uint8_t *data, unsigned n)
{
	uint8_t a[2] = { bank, offset };
	return counted(xfer(a, 2, data, n));
}

static int reg_write(uint8_t bank, uint8_t offset, const uint8_t *data, unsigned n)
{
	uint8_t b[6] = { bank, offset };
	for (unsigned i = 0; i < n && i < 4; i++)
		b[2 + i] = data[i];
	return counted(xfer(b, 2 + n, NULL, 0));
}

static void wake_line(unsigned pin)
{
	gpio_irq_enable(GPIO_WAKE, false);
	gpio_irq_ack(GPIO_WAKE);
	wake_pending = true;
}

/* boot0 high through the reset selects the ROM bootloader, then the reset
 * line is let go and the bootloader given time to listen. */
static void reset_pulse(void)
{
	gpio_set(GPIO_BOOT0, 1);
	gpio_config(GPIO_BOOT0, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	delay_ms(5);
	gpio_set(GPIO_RESET, 0);
	gpio_config(GPIO_RESET, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	delay_ms(10);
	if (V->drives_reset)
		gpio_set(GPIO_RESET, 1);
	else
		gpio_config(GPIO_RESET, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	wait_ms(BL_READY_MS);
}

static bool m4_init(void)
{
	uint8_t barker[M4_MARK_MAX];
	unsigned round;
	gpio_config(GPIO_WAKE, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	if (board->hub_pins.hold_low) {
		gpio_set(board->hub_pins.hold_low, 0);
		gpio_config(board->hub_pins.hold_low, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	}
	if (i2c_init(BUS, board->hub_pins.i2c.sda, board->hub_pins.i2c.scl, board->hub_pins.i2c.func, BUS_HZ))
		return false;
	i2c_set_timeout(BUS, STRETCH_MS);
	for (round = 0; round < BL_ROUNDS; round++) {
		reset_pulse();
		if (bl_read(V->mark_addr, barker, V->mark_size))
			break;
	}
	if (round == BL_ROUNDS) {
		log_printf("hub: m4 bootloader not answering; wake %u sda %u scl %u reset %u boot0 %u\n",
			   gpio_get(GPIO_WAKE), gpio_get(board->hub_pins.i2c.sda), gpio_get(board->hub_pins.i2c.scl),
			   gpio_get(GPIO_RESET), gpio_get(GPIO_BOOT0));
		return false;
	}
	if (!m4_barker_ok(V, barker)) {
		log_printf("hub: no firmware mark in the m4's flash, not starting it\n");
		return false;
	}
	if (!bl_opcode(M4_BL_GO) || !bl_address(V->start)) {
		log_printf("hub: m4 bootloader refused to start the firmware\n");
		return false;
	}
	state = HUB_BOOTING;
	go_t = time_ms();
	return true;
}

static void set_time(void)
{
	uint8_t b[4];
	m4_put_u32(rtc_read(), b);
	if (state == HUB_READY)
		reg_write(V->general, M4_GENERAL_UTC, b, 4);
}

/* The wake interrupts, in the hub's one register or its three. */
static int irq_enable_write(uint32_t mask)
{
	uint8_t b[4];
	m4_put_u32(mask, b);
	if (!V->byte_irqs)
		return reg_write(V->general, M4_GENERAL_WAKE_ENABLE, b, 4);
	int r = 0;
	for (unsigned i = 0; i < 3; i++)
		r |= reg_write(V->general, (uint8_t)(M4_GENERAL_INT_ENABLE + i), &b[i], 1);
	return r;
}

static int irq_read(uint8_t first, uint32_t *v)
{
	uint8_t b[4] = { 0 };
	int r = 0;
	if (!V->byte_irqs)
		r = reg_read(V->general, first, b, 4);
	else
		for (unsigned i = 0; i < 3; i++)
			r |= reg_read(V->general, (uint8_t)(first + i), &b[i], 1);
	*v = m4_u32(b);
	return r;
}

/* Sets the wake interrupts, with the gesture as tilt_wanted says, at most
 * once a second after a failure; true once they are set. A hub that is
 * told about the screen holds its gestures back while it thinks the
 * screen is on, and the gesture is wanted when it is off. */
static bool write_tilt(uint32_t now)
{
	static uint32_t tried_t;
	uint8_t screen = tilt_wanted ? M4_SCREEN_OFF : M4_SCREEN_ON;
	if (state != HUB_READY || now - tried_t < RETRY_MS)
		return false;
	if (tilt_set)
		return true;
	if (irq_enable_write(m4_irq_enable(V, tilt_wanted)) ||
	    (V->settings && reg_write(V->settings, M4_SETTINGS_SCREEN, &screen, 1))) {
		tried_t = now;
		return false;
	}
	tilt_set = true;
	return true;
}

/* The time goes to the hub, except at the start with a hub that keeps
 * the time itself: then the clock is set from it, unless the hub has
 * lost it and the clock has been set since. */
#define EPOCH_PLAUSIBLE	1400000000u

static void time_start(void)
{
	uint8_t b[4];
	if (!V->keeps_time) {
		set_time();
		return;
	}
	if (reg_read(V->general, M4_GENERAL_UTC, b, 4))
		return;
	uint32_t t = m4_u32(b);
	if (t >= EPOCH_PLAUSIBLE)
		rtc_write(t);
	else if (rtc_read() >= EPOCH_PLAUSIBLE)
		set_time();
	log_printf("hub: m4 clock %lu\n", t);
}

/* The firmware is up: the vendor driver's start, interrupts off first,
 * then the still mode timeout it uses, the time, the pedometer, and the
 * interrupts wanted. */
static void started(void)
{
	uint8_t v[2] = { 0, 0 }, zero[4] = { 0 }, on = 1, timeout[2];
	unsigned i;
	for (i = 0; i < VERSION_TRIES; i++) {
		if (!reg_read(V->general, M4_GENERAL_VERSION, v, V->version_size))
			break;
		wait_ms(100 * (i + 1));
	}
	if (i == VERSION_TRIES) {
		log_printf("hub: m4 firmware not answering\n");
		state = HUB_OFF;
		fails = FAIL_LIMIT;
		return;
	}
	version = (uint16_t)(v[0] | v[1] << 8);
	state = HUB_READY;
	if (!V->byte_irqs)
		reg_write(V->general, M4_GENERAL_NOWAKE_ENABLE, zero, 4);
	irq_enable_write(0);
	m4_put_u16(M4_STILL_TIMEOUT_S, timeout);
	reg_write(V->power, M4_POWER_STILL_TIMEOUT, timeout, 2);
	time_start();
	reg_write(V->pedometer, M4_PEDOMETER_ENABLE, &on, 1);
	reg_write(V->mets, M4_METS_ENABLE, &on, 1);
	tilt_set = profile_set = false;
	write_tilt(time_ms());
	accel_set = light_on = false;
	gpio_irq_config(GPIO_WAKE, GPIO_IRQ_LEVEL_HIGH, wake_line);
	gpio_irq_enable(GPIO_WAKE, true);
	log_printf("hub: m4 firmware %04x up\n", version);
}

static void write_accel(uint32_t now);

/* The profile, at most once a second after a failure, one register at a
 * time as the vendor writes it. */
static void write_profile(uint32_t now)
{
	static uint32_t tried_t;
	uint8_t b[4] = { profile.age, profile.male, profile.height_cm, profile.weight_kg };
	if (state != HUB_READY || !V->profile || profile_set || now - tried_t < RETRY_MS)
		return;
	for (unsigned i = 0; i < 4; i++)
		if (reg_write(V->profile, (uint8_t)(M4_PROFILE_AGE + i), &b[i], 1)) {
			tried_t = now;
			return;
		}
	profile_set = true;
}

static void m4_profile(const struct hub_profile *p)
{
	if (p->age != profile.age || p->male != profile.male || p->height_cm != profile.height_cm ||
	    p->weight_kg != profile.weight_kg) {
		profile = *p;
		profile_set = false;
	}
	write_profile(time_ms());
}

static void restart(uint32_t now, const char *why)
{
	log_printf("hub: m4 %s, restarting it\n", why);
	restarts++;
	restart_t = now;
	gpio_irq_enable(GPIO_WAKE, false);
	wake_pending = wake_retry = false;
	steps_base = steps;
	for (unsigned i = 0; i < 2; i++)
		activity_base[i] = activity[i];
	state = HUB_OFF;
	m4_init();
}

/* A wake with nothing in the status is how a crashed hub announces
 * itself; a healthy one answers the probe with 0xff bytes. */
static bool crashed(void)
{
	uint8_t p[4];
	if (reg_read(M4_BANK_PANIC, M4_PANIC_PROBE, p, 4) || !m4_panicked(p))
		return false;
	crashes++;
	return true;
}

static void m4_poll(void)
{
	i2c_poll(BUS);
	uint32_t now = time_ms();
	if (state == HUB_BOOTING && now - go_t >= BOOT_MS)
		started();
	if (state != HUB_BOOTING && fails >= FAIL_LIMIT && now - restart_t >= RESTART_MS)
		restart(now, "failing on the bus");
	if (state == HUB_READY && !tilt_set)
		write_tilt(now);
	write_accel(now);
	write_profile(now);
	if (wake_retry && now - wake_retry_t >= RETRY_MS) {
		wake_retry = false;
		gpio_irq_enable(GPIO_WAKE, true);
	}
	if (!wake_pending)
		return;
	wake_pending = false;
	wakes++;
	uint8_t g[3] = { 0 };
	uint32_t raw;
	if (irq_read(V->byte_irqs ? M4_GENERAL_INT_STATUS : M4_GENERAL_WAKE_STATUS, &raw)) {
		wake_retry = true;
		wake_retry_t = now;
		return;
	}
	uint32_t status = m4_irq_wake(V, raw);
	last_wake = raw;
	if (!raw && crashed()) {
		restart(now, "crashed");
		return;
	}
	if (status & ~M4_WAKE_GESTURE)
		log_printf("hub: m4 wake %08lx, %lu s after its start\n", raw, (now - go_t) / 1000);
	if (status & M4_WAKE_GESTURE) {
		/* One register at a time, as the vendor reads them. */
		int r = 0;
		for (unsigned i = 0; i < 3; i++)
			r |= reg_read(V->gesture, (uint8_t)(M4_GESTURE_FIRST + i), &g[i], 1);
		if (!r && m4_is_tilt(status, g[0], (int8_t)g[2]))
			tilt = true;
	}
	gpio_irq_enable(GPIO_WAKE, true);
}

static bool m4_tilt_on(bool on)
{
	if (on != tilt_wanted) {
		tilt_wanted = on;
		tilt_set = false;
	}
	return write_tilt(time_ms());
}

/* The rate is the time between samples in ms, -1 off; the hub runs it at
 * 20 ms whatever is asked. It is never switched off, see the top. */
#define ACCEL_MS	200

static uint32_t accel_t;

/* Enables the accelerometer on the hub, at most once a second after a
 * failure. */
static void write_accel(uint32_t now)
{
	static uint32_t tried_t;
	uint8_t b[2];
	if (state != HUB_READY || accel_set || now - tried_t < RETRY_MS)
		return;
	m4_put_u16(ACCEL_MS, b);
	if (reg_write(M4_BANK_ACCEL, M4_ACCEL_RATE, b, 2)) {
		tried_t = now;
		return;
	}
	accel_set = true;
	log_printf("hub: accelerometer on\n");
}

static void m4_accel_on(bool on)
{
	accel_wanted = on;
}

static bool m4_accel(int16_t v[3])
{
	uint32_t now = time_ms();
	if (!accel_wanted || !accel_set || now - accel_t < ACCEL_MS)
		return false;
	accel_t = now;
	for (unsigned i = 0; i < 3; i++) {
		uint8_t b[4];
		if (reg_read(M4_BANK_ACCEL, (uint8_t)(V->accel_x + 4 * i), b, 4))
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
	uint8_t b[M4_PEDOMETER_BLOCK_SIZE];
	if (now - steps_t >= STEPS_MS) {
		steps_t = now;
		if (!reg_read(V->pedometer, M4_PEDOMETER_BLOCK, b, sizeof b)) {
			steps = steps_base + m4_block_steps(b);
			activity[0] = activity_base[0] + m4_block_distance(b);
			if (!reg_read(V->mets, M4_METS_CALORIES, b, 4)) {
				activity[1] = activity_base[1] + m4_u32(b);
				activity_read = true;
			}
		}
	}
	return steps;
}

static bool m4_activity(uint32_t v[2])
{
	if (state != HUB_READY || !activity_read)
		return false;
	v[0] = activity[0];
	v[1] = activity[1];
	return true;
}

static bool m4_tilted(void)
{
	bool t = tilt;
	tilt = false;
	return t;
}

/* Not until the firmware is up, or the wait for it would stretch out,
 * nor with the wake line, the bus or the tilt wake still to be dealt
 * with. */
static bool m4_can_sleep(void)
{
	return state != HUB_BOOTING && !wake_pending && !wake_retry && !i2c_busy(BUS) &&
	       !(state == HUB_READY && !tilt_set);
}

static void m4_sleep(bool asleep)
{
	if (state == HUB_READY)
		wake_pin(MPM_PIN_WAKE, asleep, true);
}

static int m4_raw(uint8_t bank, uint8_t offset, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	uint8_t b[6] = { bank, offset };
	if (state != HUB_READY || wn > 4 || rn > 32)
		return -1;
	for (unsigned i = 0; i < wn; i++)
		b[2 + i] = w[i];
	return xfer(b, 2 + wn, r, rn);
}

static void m4_status(void (*say)(const char *fmt, ...))
{
	uint8_t pm[2] = { 0, 0 };
	uint32_t enable = 0;
	say("hub: m4 firmware %04x, %s, %lu steps, %lu m, %lu kcal\n", version, state == HUB_READY ? "up" : "not up",
	    steps, activity[0], activity[1]);
	say("hub: %lu wakes, last status %08lx, %lu crashes, %lu restarts, %u fails\n", wakes, last_wake, crashes,
	    restarts, fails);
	if (state == HUB_READY && !m4_raw(V->power, M4_POWER_ENABLE, NULL, 0, pm, 1) &&
	    !m4_raw(V->power, M4_POWER_STATE, NULL, 0, pm + 1, 1) &&
	    !irq_read(V->byte_irqs ? M4_GENERAL_INT_ENABLE : M4_GENERAL_WAKE_ENABLE, &enable))
		say("hub: power management %u, %s, wake interrupts %08lx\n", pm[0], pm[1] ? "moving" : "still", enable);
}

/* The hub's light sensor, where it has one: sampled by the hub at a
 * rate in ms, -1 for off, the last value read when asked. The value is
 * the hub's own measure, which the vendor's backlight code takes as it
 * is. */
#define LIGHT_MS	500

static void light_rate(bool on)
{
	uint8_t b[2];
	if (state != HUB_READY || !V->light || on == light_on)
		return;
	m4_put_u16(on ? LIGHT_MS : 0xffff, b);
	if (!reg_write(V->light, M4_LIGHT_RATE, b, 2))
		light_on = on;
}

static bool m4_light_init(void) { return true; }
static void m4_light_start(void) { light_rate(true); }

static int32_t m4_light_finish(void)
{
	uint8_t b[2];
	if (!light_on || reg_read(V->light, M4_LIGHT_VALUE, b, 2))
		return -1;
	return b[0] | b[1] << 8;
}

static void m4_light_keep(bool on)
{
	if (!on)
		light_rate(false);
}

const struct light_driver light_m4 = {
	.init = m4_light_init, .start = m4_light_start, .finish = m4_light_finish, .keep = m4_light_keep,
};

const struct hub_driver hub_m4 = {
	.init = m4_init,
	.poll = m4_poll,
	.set_time = set_time,
	.steps = m4_steps,
	.activity = m4_activity,
	.profile = m4_profile,
	.tilted = m4_tilted,
	.tilt_on = m4_tilt_on,
	.accel_on = m4_accel_on,
	.accel = m4_accel,
	.can_sleep = m4_can_sleep,
	.sleep = m4_sleep,
	.status = m4_status,
	.raw = m4_raw,
};
