/* The TicWatch Pro's sensor hub: an STM32 with its own firmware, on SPI.
 * An exchange pulls the wake line low, waits for the hub's interrupt
 * line to answer low, and with chip select held low sends a packet and
 * clocks 0xff until the answer has come: an ACK with the hub's interrupt
 * bits and then the reply, or the reply alone. The hub pulls its
 * interrupt line low when it has events, which READs fetch until it has
 * none. Four mode lines, latched by a strobe, tell the hub what the AP
 * is doing; in AMBIENT it draws the time on the second, always-on LCD,
 * and when it asks for the mode on its second line it must get one
 * within 6 s or it powers itself down. The fuel gauge is behind the hub
 * and reported as an event after a mode request. Nothing here can change
 * its flash: BOOT0 stays low, and the reset line only restarts it. */
#include <string.h>
#include "asteroid.h"
#include "hub.h"
#include "hr_accept.h"
#include "nanohub_proto.h"

#define PINS		(board->hub_pins)
#define WRITE_PAD	269
#define REPLY_BYTES	544
#define MORE_BYTES	512
#define ACK_MS		10
#define REPLY_MS	70
#define WAKE_MS		200
#define RETRIES		10
#define SPI_HZ		10000000
#define GAUGE_MS	60000
#define LIGHT_RETRY_MS	5000
#define ACCEL_RETRY_MS	5000
#define GAUGE_FORCE_MS	20000
#define READS_PER_POLL	8
#define EXTRA_PASSES	2
#define LEVEL_MS	100
#define DOWN_AFTER	3
#define RETRY_DOWN_MS	60000

enum { MODE_BAND = 1, MODE_AMBIENT = 2, MODE_NORMAL = 3, MODE_GAUGE = 5, MODE_SUSPEND = 6, MODE_RESUME = 7, MODE_GAUGE_FORCE = 8 };

static uint8_t tx[WRITE_PAD + REPLY_BYTES], rx[WRITE_PAD + REPLY_BYTES + MORE_BYTES];
static uint8_t payload[NANOHUB_PAYLOAD_MAX];
static uint32_t seq, errors, events, batched, unknown, bad, versions[2], hub_warnings, hub_errors;
static bool present, tilt_on, hr_on, accel_on, light_on, screen_off;
static int32_t lux = -1;
static volatile bool data_irq, mode_irq;
static uint8_t mode = MODE_NORMAL;
static uint32_t steps = HUB_NONE, steps_today = HUB_NONE, heart_rate = HUB_NONE;
static unsigned heart_readings;
/* The last rate kept, given out in place of the library's own for a
 * while, as a batch can end on one that is not. */
#define HR_KEPT_MS 15000
static uint32_t heart_kept = HUB_NONE, heart_kept_t;
static struct hr_accept heart_accept;
/* The trust and gain jump of a heart rate, logged when they change. */
static float hr_y, hr_z;
static bool tilted, have_gauge, flushing;
static struct hub_gauge gauge;
static uint32_t gauge_asked_t, gauge_t, level_t, failures, down_t;
static bool started, reconfigure;
static int16_t accel_v[3];
static bool have_accel;

/* The mode code on the four lines while the strobe is high, as the hub
 * latches them at its strobe interrupt. */
static void send_mode(uint8_t code)
{
	for (unsigned i = 0; i < 4; i++)
		gpio_set(PINS.mode[i], (code >> (3 - i)) & 1);
	delay_us(1200);
	if (gpio_get(PINS.strobe)) {
		gpio_set(PINS.strobe, 0);
		delay_us(1200);
	}
	gpio_set(PINS.strobe, 1);
	delay_ms(12);
	gpio_set(PINS.strobe, 0);
	delay_ms(1);
	for (unsigned i = 0; i < 4; i++)
		gpio_set(PINS.mode[i], 0);
}

static void set_mode(uint8_t code)
{
	if (code == MODE_NORMAL || code == MODE_AMBIENT || code == MODE_BAND)
		mode = code;
	send_mode(code);
}

static bool hub_irq_low(void) { return !gpio_get(PINS.irq); }

/* Finds the next packet in rx[from..to), clocking more 0xff for up to
 * ms when it has not come yet. Only a started packet is kept between
 * reads, so the buffer cannot overflow however long the hub takes. */
static int receive(unsigned *from, unsigned *to, uint32_t ms, struct nanohub_packet *p)
{
	uint32_t t0 = time_ms();
	for (;;) {
		unsigned used = 0;
		int r = *to > *from ? nanohub_decode(rx + *from, *to - *from, p, &used) : NANOHUB_NEED_MORE;
		*from += used;
		if (r == NANOHUB_OK)
			return 0;
		if (r == NANOHUB_BAD)
			continue;
		while (*from < *to && rx[*from] != NANOHUB_SYNC)
			(*from)++;
		if (*to - *from > NANOHUB_PACKET_MAX)
			(*from)++;
		if (time_ms() - t0 > ms)
			return -1;
		unsigned keep = *to - *from;
		memmove(rx, rx + *from, keep);
		*from = 0;
		*to = keep;
		if (spi_xfer(PINS.spi, NULL, rx + *to, MORE_BYTES))
			return -2;
		*to += MORE_BYTES;
	}
}

/* The wake handshake pulls the data line low as well; its edge is not
 * an event, so it is masked during an exchange and dropped after. */
static void end_exchange(void)
{
	gpio_irq_ack(PINS.irq);
	gpio_irq_enable(PINS.irq, true);
}

/* The hub powers itself off when a request for the mode goes unanswered
 * for 6 s, so it is answered between the tries of an exchange too. */
static bool answer_mode(bool level)
{
	if (!mode_irq && !(level && !gpio_get(PINS.mode_irq)))
		return false;
	mode_irq = false;
	send_mode(mode);
	reconfigure = true;
	return true;
}

/* The hub sets up for the next packet only once the last exchange has
 * ended, in an interrupt of its own. When its line is already low for
 * data, the wake handshake does not show that it did, and a packet sent
 * too soon after the wake line went high fails its CRC. */
static uint32_t turnaround_us = 1000, wake_high_us, failed_tries;

static void wake_release(void)
{
	gpio_set(PINS.wake_mcu, 1);
	wake_high_us = time_us();
}

/* One command and its reply; the reply's payload is left in payload[].
 * Returns its length, or a negative error. A hub that stops answering
 * is taken as down after a few failed exchanges, and one that does not
 * wake is not tried again within the exchange. */
static int exchange(uint32_t cmd, const uint8_t *data, unsigned len)
{
	if (!present)
		return -11;
	unsigned n = nanohub_encode(tx, ++seq, cmd, data, len);
	if (!n)
		return -10;
	memset(tx + n, 0xff, sizeof tx - n);
	gpio_irq_enable(PINS.irq, false);
	for (int attempt = 0; attempt < RETRIES; attempt++) {
		wdt_feed();
		answer_mode(true);
		bool was_low = hub_irq_low();
		gpio_set(PINS.wake_mcu, 0);
		uint32_t t0 = time_ms();
		while (!hub_irq_low() && time_ms() - t0 < WAKE_MS)
			;
		if (!hub_irq_low()) {
			wake_release();
			failed_tries++;
			log_printf("hub: %04lx try %d no wake, irq %s before\n", cmd, attempt + 1, was_low ? "low" : "high");
			break;
		}
		delay_us(40);
		uint32_t gap = time_us() - wake_high_us;
		if ((was_low || attempt) && gap < turnaround_us)
			delay_us(turnaround_us - gap);
		gpio_set(PINS.cs, 0);
		delay_us(30);
		gap = time_us() - wake_high_us;
		int r = spi_xfer(PINS.spi, tx, rx, sizeof tx);
		unsigned from = WRITE_PAD, to = sizeof tx;
		struct nanohub_packet p = { 0 };
		bool acked = false;
		if (!r)
			r = receive(&from, &to, ACK_MS, &p);
		if (!r && p.reason == NANOHUB_ACK) {
			acked = true;
			r = receive(&from, &to, REPLY_MS, &p);
		}
		gpio_set(PINS.cs, 1);
		wake_release();
		delay_us(60);
		if (!r && p.seq == seq && p.reason == cmd) {
			memcpy(payload, p.payload, p.len);
			end_exchange();
			failures = 0;
			return p.len;
		}
		const char *why = "bad reply";
		if (r)
			why = r == -2 ? "spi error" : acked ? "no reply" : "no ACK";
		else if (p.reason == NANOHUB_NAK || p.reason == NANOHUB_BUSY)
			why = p.reason == NANOHUB_NAK ? "NAK" : "busy";
		failed_tries++;
		log_printf("hub: %04lx try %d %s, irq %s before wake, clock %lu us after the last\n", cmd, attempt + 1, why,
			   was_low ? "low" : "high", gap);
		if (!r && p.reason == NANOHUB_BUSY) {
			delay_ms(100);
			wdt_feed();
		}
	}
	end_exchange();
	errors++;
	if (++failures >= DOWN_AFTER && present) {
		present = false;
		down_t = time_ms();
		log_printf("hub: not answering, link down\n");
	}
	return -1;
}

/* Whether the hub took the setting: its reply is one byte, 1 if so. */
static bool configure(uint8_t type, uint8_t cmd, uint32_t rate, uint64_t latency_ns)
{
	uint8_t b[NANOHUB_PAYLOAD_MAX];
	unsigned n = nanohub_sensor_config(b, type, cmd, rate, latency_ns, NULL, 0);
	return present && exchange(NANOHUB_WRITE, b, n) >= 1 && payload[0];
}

static void set_time(void)
{
	uint8_t b[NANOHUB_PAYLOAD_MAX];
	/* The firmware keeps local time, so the hub gets it with no offset. */
	unsigned n = nanohub_time_config(b, rtc_read(), 0, !setting_get(SETTING_CLOCK12));
	if (present && exchange(NANOHUB_WRITE, b, n) < 0)
		log_printf("hub: time not set\n");
}

static void sensor_event(const struct nanohub_event *ev)
{
	const uint8_t *d;
	uint64_t ns;
	unsigned last = ev->sensor.samples ? ev->sensor.samples - 1u : 0;
	switch (ev->sensor.type) {
	case NANOHUB_SENSOR_HAND_UP:
		tilted = true;
		break;
	case NANOHUB_SENSOR_STEPS:
		if (ev->sensor.header && nanohub_sample(ev, last, 4, &ns, &d))
			memcpy(&steps, d, 4);
		break;
	case NANOHUB_SENSOR_STEPS_TODAY:
		if (ev->sensor.header && nanohub_sample(ev, last, 4, &ns, &d))
			memcpy(&steps_today, d, 4);
		flushing = false;
		break;
	case NANOHUB_SENSOR_HEART_RATE:
		for (unsigned i = 0; ev->sensor.header && nanohub_sample(ev, i, 12, &ns, &d); i++) {
			float bpm;
			memcpy(&bpm, d, 4);
			float y, z;
			memcpy(&y, d + 4, 4);
			memcpy(&z, d + 8, 4);
			if (y != hr_y || z != hr_z)
				log_printf("hub: heart rate %ld, y %ld, z %ld\n", (long)(bpm + 0.5f), (long)y, (long)z);
			hr_y = y;
			hr_z = z;
			heart_rate = bpm > 0 ? (uint32_t)(bpm + 0.5f) : HUB_NONE;
			unsigned kept = hr_accept_take(&heart_accept, bpm, y, z, time_ms());
			if (kept) {
				heart_kept = kept;
				heart_kept_t = time_ms();
				heart_readings++;
			}
		}
		break;
	case NANOHUB_SENSOR_LIGHT:
		if (ev->sensor.header && nanohub_sample(ev, last, 4, &ns, &d)) {
			float v;
			memcpy(&v, d, 4);
			lux = v >= 0 ? (int32_t)(v + 0.5f) : -1;
		}
		break;
	case NANOHUB_SENSOR_ACCEL_RAW:
		if (ev->sensor.header && nanohub_sample(ev, last, 6, &ns, &d)) {
			memcpy(accel_v, d, 6);
			have_accel = true;
		}
		break;
	default:
		break;
	}
}

static void log_event(const struct nanohub_event *ev);
static bool flash_cmd(uint8_t sub, uint8_t arg);
static void mask_update(void);

static void handle_event(const struct nanohub_event *ev)
{
	events++;
	if (ev->kind == NANOHUB_EV_APP)
		log_event(ev);
	if (ev->kind == NANOHUB_EV_SENSOR)
		sensor_event(ev);
	else if (ev->kind == NANOHUB_EV_FUEL_GAUGE) {
		gauge_t = time_ms();
		gauge = (struct hub_gauge){ ev->gauge.mv, ev->gauge.percent, -ev->gauge.current_ma * 1000, ev->gauge.temp_dc,
					    gauge_t };
		have_gauge = true;
		gauge_asked_t = 0;
	} else if (ev->kind == NANOHUB_EV_LOG) {
		char line[96];
		unsigned k = ev->log.size < sizeof line - 1 ? ev->log.size : sizeof line - 1;
		memcpy(line, ev->log.text, k);
		line[k] = 0;
		log_printf("hub: %c %s\n", ev->log.level, line);
		if (ev->log.level == 'W')
			hub_warnings++;
		else if (ev->log.level == 'E')
			hub_errors++;
	}
}

/* Fetches events until the hub has none, a few per pass, and then
 * clears its two data-pending bits so it lets its line go. One answer
 * can hold several sensor events, a hand-up behind the steps. False if
 * an exchange failed. */
static bool read_events(void)
{
	for (unsigned i = 0; i < READS_PER_POLL; i++) {
		uint8_t b[8];
		unsigned n = nanohub_read_payload(b, (uint64_t)time_us() * 1000);
		int len = exchange(NANOHUB_READ, b, n);
		struct nanohub_event ev;
		if (len <= 0 || !nanohub_event(payload, (unsigned)len, &ev) || ev.kind == NANOHUB_EV_EMPTY) {
			uint8_t clear[NANOHUB_ACK_INTR_SIZE] = { 6 };
			return exchange(NANOHUB_CLR_GET_INTR, clear, sizeof clear) >= 0 && len >= 0;
		}
		struct nanohub_walk w = { 0 };
		unsigned k = 0;
		while (nanohub_next_event(payload, (unsigned)len, &w, &ev)) {
			handle_event(&ev);
			k++;
		}
		if (k > 1)
			batched++;
		unknown += w.unknown;
		bad += w.bad;
	}
	return true;
}

/* The log the hub keeps in band mode (Timepiece): read once per boot, a
 * chunk per pass, its heart rate readings kept for the app to take; it
 * is erased only at the next Timepiece, and only once read. */
#define LOG_SAMPLES		64
#define LOG_REPLY_MS		300
#define LOG_TRIES		3
#define ERASE_STALL_MS		1500
#define ERASE_WAIT_MS		3500
enum { LOG_START, LOG_WAIT, LOG_PAUSED, LOG_DONE, LOG_FAILED };
static uint8_t log_state = LOG_FAILED, log_tries;
static uint32_t log_t, log_records;
static uint32_t log_time[LOG_SAMPLES];
static uint8_t log_bpm[LOG_SAMPLES];
static unsigned log_head, log_tail;
static bool erased;

static bool flash_cmd(uint8_t sub, uint8_t arg)
{
	uint8_t c[2] = { sub, arg }, b[NANOHUB_PAYLOAD_MAX];
	unsigned n = nanohub_sensor_config(b, NANOHUB_SENSOR_CUSTOM_FLASH, NANOHUB_CMD_CFG_DATA, 0, 0, c, sizeof c);
	return present && exchange(NANOHUB_WRITE, b, n) >= 0;
}

static void log_request(void)
{
	if (LOG_SAMPLES - (log_head - log_tail) < 10) {
		log_state = LOG_PAUSED;
		return;
	}
	mask_update();
	if (flash_cmd(1, 1)) {
		log_state = LOG_WAIT;
		log_t = time_ms();
	} else {
		log_state = LOG_FAILED;
	}
}

static void log_event(const struct nanohub_event *ev)
{
	const uint8_t *p;
	bool last;
	int n = nanohub_log_chunk(ev, &p, &last);
	if (nanohub_log_erased(ev))
		erased = true;
	if (n < 0 || log_state != LOG_WAIT)
		return;
	for (int i = 0; i < n; i += NANOHUB_LOG_RECORD) {
		struct nanohub_log_record r;
		if (!nanohub_log_record(p + i, &r))
			continue;
		log_records++;
		if (r.bpm && log_head - log_tail < LOG_SAMPLES) {
			log_time[log_head % LOG_SAMPLES] = r.time;
			log_bpm[log_head % LOG_SAMPLES] = r.bpm;
			log_head++;
		}
	}
	log_tries = 0;
	if (last) {
		log_state = LOG_DONE;
		log_printf("hub: band log read, %lu records\n", log_records);
	} else {
		log_request();
	}
}

static void log_poll(void)
{
	if (log_state == LOG_START || (log_state == LOG_PAUSED && LOG_SAMPLES - (log_head - log_tail) >= 10)) {
		log_request();
	} else if (log_state == LOG_WAIT && time_ms() - log_t >= LOG_REPLY_MS) {
		/* A request the hub got while busy is dropped without a reply. */
		if (++log_tries > LOG_TRIES) {
			log_state = LOG_FAILED;
			log_printf("hub: band log not read\n");
		} else {
			log_request();
		}
	}
}

static unsigned nanohub_heart_log(uint32_t *t, uint8_t *bpm, unsigned max)
{
	unsigned n = 0;
	for (; n < max && log_tail != log_head; n++, log_tail++) {
		t[n] = log_time[log_tail % LOG_SAMPLES];
		bpm[n] = log_bpm[log_tail % LOG_SAMPLES];
	}
	return n;
}

/* Erasing stalls the hub for up to a second, which looks like a hub that
 * does not answer; it signals the end with an event. */
static void log_erase(void)
{
	if (log_state != LOG_DONE || !flash_cmd(1, 0))
		return;
	erased = false;
	for (uint32_t ms = 0; ms < ERASE_STALL_MS; ms += 10) {
		wdt_feed();
		delay_ms(10);
	}
	for (uint32_t t0 = time_ms(); !erased && time_ms() - t0 < ERASE_WAIT_MS;) {
		wdt_feed();
		answer_mode(true);
		if (hub_irq_low())
			read_events();
		delay_ms(20);
	}
	log_printf("hub: band log %s\n", erased ? "erased" : "erase not confirmed");
}

static void data_edge(unsigned n)
{
	gpio_irq_ack(n);
	data_irq = true;
}

static void mode_edge(unsigned n)
{
	gpio_irq_ack(n);
	mode_irq = true;
}

static void out(uint8_t pin, unsigned v)
{
	gpio_set(pin, v);
	gpio_config(pin, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
}

/* What the hub should be doing, sent again after it restarted. */
static bool want_tilt, want_hr, want_accel, want_light;

/* Every log line and sensor batch of the hub pulls its one line to the
 * AP, which wakes it. As the vendor kernel does at suspend, its
 * non-wakeup interrupt is masked while the watch sleeps, unless
 * something waits for such data: the sensors on, a flush of the steps,
 * the fuel gauge, the band log, or Timepiece. The tilt, touch and app messages still
 * come, and a read takes in whatever else is waiting. The hub starts
 * unmasked. */
#define FLUSH_MS	2000
#define MASK_TRIES	3
#define MASK_RETRY_MS	1000
static bool asleep, ending, mask_on = true, masked, mask_known, mask_failed;
static uint32_t flush_t, mask_t, mask_errors;

static bool mask_wanted(void)
{
	uint32_t now = time_ms();
	if (!mask_on || !asleep || ending || want_hr || hr_on || want_light || light_on || want_accel || accel_on)
		return false;
	if (log_state == LOG_START || log_state == LOG_WAIT)
		return false;
	if (flushing && now - flush_t < FLUSH_MS)
		return false;
	return !gauge_asked_t || now - gauge_asked_t >= GAUGE_FORCE_MS;
}

/* Called wherever what it depends on changes, and on every poll, so a
 * change the hub did not take is tried again. */
static void mask_update(void)
{
	bool want = mask_wanted();
	if (!present || (mask_known && want == masked) || (mask_failed && time_ms() - mask_t < MASK_RETRY_MS))
		return;
	uint8_t b[1] = { NANOHUB_INT_NONWAKEUP };
	for (int i = 0; i < MASK_TRIES; i++) {
		int len = exchange(want ? NANOHUB_MASK_INTR : NANOHUB_UNMASK_INTR, b, sizeof b);
		if (len >= 1 && payload[0] == 1) {
			masked = want;
			mask_known = true;
			mask_failed = false;
			return;
		}
		if (len < 0)
			break;
	}
	mask_known = false;
	mask_failed = true;
	mask_t = time_ms();
	mask_errors++;
	log_printf("hub: interrupt %s not taken\n", want ? "mask" : "unmask");
}

/* The steps come once a minute even when they don't change, and every
 * event wakes the AP: with the screen off the hub holds them back for
 * up to an hour, until the screen turns on or the day ends. */
static uint64_t steps_latency_ns(void)
{
	return screen_off ? 3600000000000ull : 0;
}

/* A heart rate comes about once a second, and with the screen off each
 * one woke the AP for the 30 to 60 s a reading takes. */
static uint64_t hr_latency_ns(void)
{
	return screen_off ? 10000000000ull : 0;
}

static void nanohub_flush_steps(void)
{
	flushing = true;
	flush_t = time_ms();
	mask_update();
	configure(NANOHUB_SENSOR_STEPS, NANOHUB_CMD_FLUSH, 0, 0);
}

/* The count since the hub started is what the watch counts from, as on
 * every watch; the hub's own steps today, which it shows on its LCD, is
 * read once after boot for the steps taken while the watch was off. */
static void enable_steps(void)
{
	if (!configure(NANOHUB_SENSOR_STEPS, NANOHUB_CMD_ENABLE, NANOHUB_RATE_ON_CHANGE, steps_latency_ns()))
		log_printf("hub: step counter not enabled\n");
	if (!configure(NANOHUB_SENSOR_STEPS_TODAY, NANOHUB_CMD_ENABLE, NANOHUB_RATE_ON_CHANGE, steps_latency_ns()))
		log_printf("hub: steps today not enabled\n");
}

static void apply_state(void)
{
	mask_known = false;
	set_time();
	enable_steps();
	/* What the hub logs in band mode: steps and activity, and the heart
	 * rate if the watch logs it. */
	flash_cmd(4, 1);
	flash_cmd(3, 1);
	flash_cmd(2, setting_get(SETTING_HR_INTERVAL) > 0);
	/* The steps row on the hub's display. */
	static const uint8_t steps_row[26] = { 0, 1, 1, 1 };
	uint8_t b[NANOHUB_PAYLOAD_MAX];
	unsigned n = nanohub_sensor_config(b, NANOHUB_SENSOR_LCD_A, NANOHUB_CMD_CFG_DATA, 0, 0, steps_row, sizeof steps_row);
	if (present && exchange(NANOHUB_WRITE, b, n) < 0)
		log_printf("hub: steps row not set\n");
	tilt_on = want_tilt && configure(NANOHUB_SENSOR_HAND_UP, NANOHUB_CMD_ENABLE, NANOHUB_RATE_ON_CHANGE, 0);
	hr_on = want_hr && configure(NANOHUB_SENSOR_HEART_RATE, NANOHUB_CMD_ENABLE, NANOHUB_RATE_ON_CHANGE, hr_latency_ns());
	accel_on = want_accel && configure(NANOHUB_SENSOR_ACCEL, NANOHUB_CMD_ENABLE, NANOHUB_HZ(10), 0);
	light_on = want_light && configure(NANOHUB_SENSOR_LIGHT, NANOHUB_CMD_ENABLE, NANOHUB_RATE_ON_CHANGE, 0);
	if (asleep)
		send_mode(MODE_SUSPEND);
	mask_update();
}

static bool answers(void)
{
	present = true;
	failures = 0;
	int len = exchange(NANOHUB_GET_VERSIONS, NULL, 0);
	if (len < 0)
		return false;
	if (len >= 8)
		memcpy(versions, payload, 8);
	log_printf("hub: nanohub hw %08lx fw %08lx\n", versions[0], versions[1]);
	return true;
}

/* The mode lines work without the SPI link, so the driver stays in use
 * and answers the hub's requests for the mode whatever else fails. */
static bool nanohub_init(void)
{
	out(PINS.boot0, 0);
	out(PINS.cs, 1);
	out(PINS.wake_mcu, 1);
	wake_high_us = time_us();
	out(PINS.strobe, 0);
	for (unsigned i = 0; i < 4; i++)
		out(PINS.mode[i], 0);
	gpio_config(PINS.irq, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	gpio_config(PINS.mode_irq, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	gpio_irq_config(PINS.irq, GPIO_IRQ_EDGE_FALLING, data_edge);
	gpio_irq_config(PINS.mode_irq, GPIO_IRQ_EDGE_FALLING, mode_edge);
	gpio_irq_enable(PINS.mode_irq, true);
	started = true;
	const uint8_t spi_pins[3] = { PINS.gpio, PINS.gpio + 1, PINS.gpio + 3 };
	if (spi_init(PINS.spi, spi_pins, PINS.func, 0, SPI_HZ)) {
		log_printf("hub: no spi\n");
		down_t = time_ms();
		return true;
	}
	/* A restart of its firmware, as at every boot of the watch's own
	 * system: with BOOT0 low the reset line does nothing else. */
	out(PINS.reset, 1);
	delay_us(40);
	gpio_set(PINS.reset, 0);
	for (int i = 0; i < 80; i++) {
		delay_ms(10);
		wdt_feed();
	}
	set_mode(MODE_NORMAL);
	gpio_irq_enable(PINS.irq, true);
	if (!answers()) {
		present = false;
		down_t = time_ms();
		log_printf("hub: no answer, irq %u\n", gpio_get(PINS.irq));
		return true;
	}
	set_mode(mode);
	apply_state();
	data_irq = true;
	log_state = LOG_START;
	return true;
}

static void poll_gauge(void)
{
	if (!present)
		return;
	uint32_t now = time_ms();
	if (gauge_asked_t && now - gauge_asked_t >= GAUGE_FORCE_MS) {
		gauge_asked_t = now;
		mask_update();
		send_mode(MODE_GAUGE_FORCE);
	} else if (!gauge_asked_t && (!have_gauge || now - gauge_t >= GAUGE_MS)) {
		gauge_asked_t = now;
		mask_update();
		send_mode(MODE_GAUGE);
	}
}

/* The edges are what the main loop waits for; the levels catch one
 * that was missed, at most every LEVEL_MS. A request for the mode means
 * the hub (re)started, so it gets its settings again too. A hub that
 * went quiet is tried again every RETRY_DOWN_MS. */
static void nanohub_poll(void)
{
	if (!started)
		return;
	uint32_t now = time_ms();
	bool level_due = now - level_t >= LEVEL_MS;
	if (level_due)
		level_t = now;
	answer_mode(level_due);
	if (!present) {
		if (down_t && now - down_t >= RETRY_DOWN_MS) {
			down_t = now;
			if (answers())
				reconfigure = true;
			else
				present = false;
		}
		if (!present)
			return;
	}
	if (reconfigure) {
		reconfigure = false;
		apply_state();
	}
	/* A line still low after a pass is data that came in meanwhile,
	 * read right away rather than at the next level check. */
	if (data_irq || (level_due && hub_irq_low())) {
		data_irq = false;
		for (unsigned i = 0; read_events() && i < EXTRA_PASSES && hub_irq_low(); i++)
			;
	}
	log_poll();
	/* With the screen off the charger asks when it needs a reading. */
	if (!screen_off)
		poll_gauge();
	mask_update();
}

static bool get_gauge(struct hub_gauge *g)
{
	if (have_gauge)
		*g = gauge;
	return have_gauge;
}

static uint32_t nanohub_steps(void) { return steps; }
static uint32_t nanohub_steps_today(void) { return steps_today; }

/* What the hub's LCD shows as steps today. */
static void nanohub_sync_steps_today(uint32_t v)
{
	uint8_t b[NANOHUB_PAYLOAD_MAX];
	unsigned n = nanohub_sensor_config(b, NANOHUB_SENSOR_STEPS_TODAY, NANOHUB_CMD_CFG_DATA, 0, 0, (const uint8_t *)&v, 4);
	if (present && exchange(NANOHUB_WRITE, b, n) >= 0)
		steps_today = v;
}

static void nanohub_heart_rate_on(bool on)
{
	want_hr = on;
	if (on == hr_on)
		return;
	mask_update();
	heart_rate = HUB_NONE;
	heart_readings = 0;
	heart_kept = HUB_NONE;
	hr_accept_reset(&heart_accept);
	if (configure(NANOHUB_SENSOR_HEART_RATE, on ? NANOHUB_CMD_ENABLE : NANOHUB_CMD_DISABLE, NANOHUB_RATE_ON_CHANGE, hr_latency_ns()))
		hr_on = on;
	else
		log_printf("hub: heart rate %s not taken\n", on ? "on" : "off");
	mask_update();
}

static uint32_t nanohub_heart_rate(uint8_t *status, unsigned *readings)
{
	if (status)
		*status = 0;
	if (readings)
		*readings = hr_on ? heart_readings : 0;
	if (heart_kept != HUB_NONE && time_ms() - heart_kept_t < HR_KEPT_MS)
		return heart_kept;
	return heart_rate;
}

static bool nanohub_tilted(void)
{
	bool t = tilted;
	tilted = false;
	return t;
}

static bool nanohub_tilt_on(bool on)
{
	want_tilt = on;
	if (on == tilt_on)
		return true;
	if (!configure(NANOHUB_SENSOR_HAND_UP, on ? NANOHUB_CMD_ENABLE : NANOHUB_CMD_DISABLE, NANOHUB_RATE_ON_CHANGE, 0))
		return false;
	tilt_on = on;
	return true;
}

/* Asked each pass: a refused change waits ACCEL_RETRY_MS before the
 * next try, as each one blocks on the hub. */
static void nanohub_accel_on(bool on)
{
	static uint32_t refused_t;
	static bool refused;
	want_accel = on;
	if (on == accel_on)
		return;
	uint32_t now = time_ms();
	if (refused && now - refused_t < ACCEL_RETRY_MS)
		return;
	have_accel = false;
	mask_update();
	refused = !configure(NANOHUB_SENSOR_ACCEL, on ? NANOHUB_CMD_ENABLE : NANOHUB_CMD_DISABLE, NANOHUB_HZ(10), 0);
	refused_t = now;
	if (!refused)
		accel_on = on;
	mask_update();
}

static bool nanohub_accel(int16_t v[3])
{
	if (!accel_on || !have_accel)
		return false;
	memcpy(v, accel_v, sizeof accel_v);
	have_accel = false;
	return true;
}

/* Not with events waiting: in WFI only a new edge would wake the CPU. */
static bool nanohub_can_sleep(void)
{
	return !present || (!data_irq && !mode_irq && !hub_irq_low());
}

/* With the cluster down only the MPM sees the hub's lines. The data line
 * stays low until it is read, the mode ask is a pulse. */
static void nanohub_sleep(bool asleep)
{
	wake_pin(PINS.mpm, asleep, false);
	wake_pin_falling(PINS.mode_mpm, asleep);
}

/* As the vendor kernel does at suspend and resume: masked, then
 * SUSPEND, and RESUME before it is unmasked. */
static void nanohub_asleep(bool a)
{
	asleep = a;
	if (asleep)
		mask_update();
	if (present)
		send_mode(asleep ? MODE_SUSPEND : MODE_RESUME);
	if (!asleep)
		mask_update();
}

/* With the screen off the hub draws the time on its own LCD. */
static void nanohub_screen(bool on)
{
	screen_off = !on;
	if (!started)
		return;
	set_mode(on ? MODE_NORMAL : MODE_AMBIENT);
	enable_steps();
	if (hr_on)
		configure(NANOHUB_SENSOR_HEART_RATE, NANOHUB_CMD_ENABLE, NANOHUB_RATE_ON_CHANGE, hr_latency_ns());
	if (on) {
		nanohub_flush_steps();
		if (hr_on)
			configure(NANOHUB_SENSOR_HEART_RATE, NANOHUB_CMD_FLUSH, 0, 0);
	}
}

/* What the vendor's Essential mode leaves the hub in: its sensors for the
 * watch off, the idle workout frame, then BAND, in which it shows the
 * time, date, battery and steps until the next boot. */

static void nanohub_timepiece(void)
{
	if (!started)
		return;
	ending = true;
	mask_update();
	want_tilt = want_hr = want_accel = false;
	tilt_on = hr_on = accel_on = false;
	if (present) {
		configure(NANOHUB_SENSOR_HAND_UP, NANOHUB_CMD_DISABLE, 0, 0);
		configure(NANOHUB_SENSOR_HEART_RATE, NANOHUB_CMD_DISABLE, 0, 0);
		configure(NANOHUB_SENSOR_ACCEL, NANOHUB_CMD_DISABLE, 0, 0);
		uint8_t frame[26] = { 1, 5 };
		frame[25] = 1;
		uint8_t b[NANOHUB_PAYLOAD_MAX];
		unsigned n = nanohub_sensor_config(b, NANOHUB_SENSOR_LCD_A, NANOHUB_CMD_CFG_DATA, 0, 0, frame, sizeof frame);
		if (exchange(NANOHUB_WRITE, b, n) < 0)
			log_printf("hub: idle frame not set\n");
	}
	set_mode(MODE_AMBIENT);
	log_erase();
	set_mode(MODE_BAND);
	log_printf("hub: timepiece\n");
}

/* The workout frame of the hub's display (frame 6: time, heart rate,
 * calories, distance and pace), shown in ambient mode; the hub runs the
 * clock itself, so changes go at most every WORKOUT_MS. The end puts the
 * idle frame back. */
#define WORKOUT_MS		10000
#define FRAME_IDLE		5
#define FRAME_WORKOUT		6
#define STATE_RUNNING		2
#define STATE_STOPPED		4

static bool workout_shown;
static uint32_t workout_t;

static void nanohub_workout(bool running, uint32_t seconds, uint32_t distance_m, uint32_t kcal)
{
	uint32_t now = time_ms();
	if (!present || (!running && !workout_shown) ||
	    (running && workout_shown && now - workout_t < WORKOUT_MS))
		return;
	uint8_t f[26] = { 1, running ? FRAME_WORKOUT : FRAME_IDLE, running ? STATE_RUNNING : STATE_STOPPED };
	if (running) {
		uint32_t ms = seconds * 1000, pace = distance_m ? seconds * 1000 / distance_m : 0;
		float km = (float)distance_m / 1000.0f;
		f[5] = heart_rate != HUB_NONE && heart_rate < 256 ? (uint8_t)heart_rate : 0;
		memcpy(f + 6, &ms, 4);
		memcpy(f + 10, &kcal, 4);
		memcpy(f + 14, &km, 4);
		memcpy(f + 18, &pace, 4);
	}
	f[25] = 1;
	uint8_t b[NANOHUB_PAYLOAD_MAX];
	unsigned n = nanohub_sensor_config(b, NANOHUB_SENSOR_LCD_A, NANOHUB_CMD_CFG_DATA, 0, 0, f, sizeof f);
	if (exchange(NANOHUB_WRITE, b, n) < 0)
		return;
	workout_shown = running;
	workout_t = now;
}

static int nanohub_mode(unsigned code)
{
	if (!started || (code != MODE_BAND && code != MODE_AMBIENT && code != MODE_NORMAL))
		return -1;
	set_mode((uint8_t)code);
	return 0;
}

static void nanohub_status(void (*say)(const char *fmt, ...))
{
	say("hub: nanohub hw %08lx fw %08lx, %s, mode %u, %lu events (%lu answers with more, %lu unknown sizes, %lu cut "
	    "short), %lu errors\n",
	    versions[0], versions[1], present ? "up" : "not up", mode, events, batched, unknown, bad, errors);
	say("hub: its own log %lu warnings and %lu errors, steps %ld today %ld, gauge %s %lu mV %lu%%\n", hub_warnings, hub_errors,
	    (long)steps, (long)steps_today, have_gauge ? "" : "none", gauge.mv, gauge.percent);
	say("hub: non-wakeup interrupt %s%s, %lu mask errors, %lu failed tries, turnaround %lu us\n",
	    !mask_known ? "unknown" : masked ? "masked" : "unmasked", mask_on ? "" : " (masking off)", mask_errors,
	    failed_tries, turnaround_us);
}

/* mask 0 keeps the non-wakeup interrupt unmasked asleep, and
 * turnaround is the least time from the end of an exchange to the next
 * packet, for comparing. */
static int nanohub_option(const char *name, uint32_t v)
{
	if (!strcmp(name, "mask")) {
		mask_on = v;
		mask_update();
		return 0;
	}
	if (!strcmp(name, "turnaround")) {
		turnaround_us = v;
		return 0;
	}
	return -1;
}

/* The hub's light sensor: on while it is wanted, the last lux it
 * reported when asked. A refused enable waits LIGHT_RETRY_MS rather than
 * a reading, as each try holds the main loop until the hub answers. */
static void light_set(bool on)
{
	static uint32_t refused_t;
	static bool refused;
	bool wanted = want_light;
	want_light = on;
	/* After a refused disable the last lux may be from long ago. */
	if (on && !wanted)
		lux = -1;
	if (on == light_on)
		return;
	if (!on)
		lux = -1;
	uint32_t now = time_ms();
	if (on && refused && now - refused_t < LIGHT_RETRY_MS)
		return;
	mask_update();
	if (configure(NANOHUB_SENSOR_LIGHT, on ? NANOHUB_CMD_ENABLE : NANOHUB_CMD_DISABLE, NANOHUB_RATE_ON_CHANGE, 0))
		light_on = on;
	refused = on && !light_on;
	refused_t = now;
	mask_update();
}

static bool nanohub_light_init(void) { return true; }
static void nanohub_light_start(void) { light_set(true); }
static int32_t nanohub_light_finish(void) { return light_on ? lux : -1; }
static void nanohub_light_keep(bool on) { light_set(on); }

const struct light_driver light_nanohub = {
	.init = nanohub_light_init,
	.start = nanohub_light_start,
	.finish = nanohub_light_finish,
	.keep = nanohub_light_keep,
};

const struct hub_driver hub_nanohub = {
	.init = nanohub_init,
	.poll = nanohub_poll,
	.set_time = set_time,
	.steps = nanohub_steps,
	.steps_today = nanohub_steps_today,
	.sync_steps_today = nanohub_sync_steps_today,
	.flush_steps = nanohub_flush_steps,
	.heart_rate_on = nanohub_heart_rate_on,
	.heart_rate = nanohub_heart_rate,
	.tilted = nanohub_tilted,
	.tilt_on = nanohub_tilt_on,
	.accel_on = nanohub_accel_on,
	.accel = nanohub_accel,
	.can_sleep = nanohub_can_sleep,
	.sleep = nanohub_sleep,
	.asleep = nanohub_asleep,
	.option = nanohub_option,
	.screen = nanohub_screen,
	.status = nanohub_status,
	.gauge = get_gauge,
	.poll_gauge = poll_gauge,
	.timepiece = nanohub_timepiece,
	.workout = nanohub_workout,
	.heart_log = nanohub_heart_log,
	.mode = nanohub_mode,
};
