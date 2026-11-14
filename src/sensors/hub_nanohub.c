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
#define GAUGE_FORCE_MS	20000
#define READS_PER_POLL	8
#define LEVEL_MS	100
#define DOWN_AFTER	3
#define RETRY_DOWN_MS	60000

enum { MODE_BAND = 1, MODE_AMBIENT = 2, MODE_NORMAL = 3, MODE_GAUGE = 5, MODE_GAUGE_FORCE = 8 };

static uint8_t tx[WRITE_PAD + REPLY_BYTES], rx[WRITE_PAD + REPLY_BYTES + MORE_BYTES];
static uint8_t payload[NANOHUB_PAYLOAD_MAX];
static uint32_t seq, errors, events, versions[2];
static bool present, tilt_on, hr_on;
static volatile bool data_irq, mode_irq;
static uint8_t mode = MODE_NORMAL;
static uint32_t steps_today = HUB_NONE, heart_rate = HUB_NONE;
static bool tilted, have_gauge;
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
		gpio_set(PINS.wake_mcu, 0);
		uint32_t t0 = time_ms();
		while (!hub_irq_low() && time_ms() - t0 < WAKE_MS)
			;
		if (!hub_irq_low()) {
			gpio_set(PINS.wake_mcu, 1);
			break;
		}
		delay_us(40);
		gpio_set(PINS.cs, 0);
		delay_us(30);
		int r = spi_xfer(PINS.spi, tx, rx, sizeof tx);
		unsigned from = WRITE_PAD, to = sizeof tx;
		struct nanohub_packet p = { 0 };
		if (!r)
			r = receive(&from, &to, ACK_MS, &p);
		if (!r && p.reason == NANOHUB_ACK)
			r = receive(&from, &to, REPLY_MS, &p);
		gpio_set(PINS.cs, 1);
		gpio_set(PINS.wake_mcu, 1);
		delay_us(60);
		if (!r && p.seq == seq && p.reason == cmd) {
			memcpy(payload, p.payload, p.len);
			end_exchange();
			failures = 0;
			return p.len;
		}
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
	case NANOHUB_SENSOR_STEPS_TODAY:
		if (ev->sensor.header && nanohub_sample(ev, last, 4, &ns, &d))
			memcpy(&steps_today, d, 4);
		break;
	case NANOHUB_SENSOR_HEART_RATE:
		if (ev->sensor.header && nanohub_sample(ev, last, 12, &ns, &d)) {
			float bpm;
			memcpy(&bpm, d, 4);
			heart_rate = bpm > 0 ? (uint32_t)(bpm + 0.5f) : HUB_NONE;
		}
		break;
	case NANOHUB_SENSOR_ACCEL:
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

/* Fetches events until the hub has none, a few per pass, and then
 * clears its two data-pending bits so it lets its line go. */
static void read_events(void)
{
	for (unsigned i = 0; i < READS_PER_POLL; i++) {
		uint8_t b[8];
		unsigned n = nanohub_read_payload(b, (uint64_t)time_us() * 1000);
		int len = exchange(NANOHUB_READ, b, n);
		struct nanohub_event ev;
		if (len <= 0 || !nanohub_event(payload, (unsigned)len, &ev) || ev.kind == NANOHUB_EV_EMPTY) {
			uint8_t clear[NANOHUB_ACK_INTR_SIZE] = { 6 };
			exchange(NANOHUB_CLR_GET_INTR, clear, sizeof clear);
			break;
		}
		events++;
		if (ev.kind == NANOHUB_EV_APP)
			log_event(&ev);
		if (ev.kind == NANOHUB_EV_SENSOR)
			sensor_event(&ev);
		else if (ev.kind == NANOHUB_EV_FUEL_GAUGE) {
			gauge = (struct hub_gauge){ ev.gauge.mv, ev.gauge.percent, -ev.gauge.current_ma * 1000, ev.gauge.temp_dc };
			have_gauge = true;
			gauge_t = time_ms();
			gauge_asked_t = 0;
		} else if (ev.kind == NANOHUB_EV_LOG) {
			char line[96];
			unsigned k = ev.log.size < sizeof line - 1 ? ev.log.size : sizeof line - 1;
			memcpy(line, ev.log.text, k);
			line[k] = 0;
			log_printf("hub: %c %s\n", ev.log.level, line);
		}
	}
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
static bool want_tilt, want_hr, want_accel;

static void apply_state(void)
{
	set_time();
	if (!configure(NANOHUB_SENSOR_STEPS_TODAY, NANOHUB_CMD_ENABLE, NANOHUB_RATE_ON_CHANGE, 0))
		log_printf("hub: steps today not enabled\n");
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
	hr_on = want_hr && configure(NANOHUB_SENSOR_HEART_RATE, NANOHUB_CMD_ENABLE, NANOHUB_RATE_ON_CHANGE, 0);
	if (want_accel)
		configure(NANOHUB_SENSOR_ACCEL, NANOHUB_CMD_ENABLE, NANOHUB_HZ(10), 0);
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
		send_mode(MODE_GAUGE_FORCE);
		gauge_asked_t = now;
	} else if (!gauge_asked_t && (!have_gauge || now - gauge_t >= GAUGE_MS)) {
		send_mode(MODE_GAUGE);
		gauge_asked_t = now;
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
	if (data_irq || (level_due && hub_irq_low())) {
		data_irq = false;
		read_events();
	}
	log_poll();
	poll_gauge();
}

static bool get_gauge(struct hub_gauge *g)
{
	if (have_gauge)
		*g = gauge;
	return have_gauge;
}

static uint32_t nanohub_steps_today(void) { return steps_today; }

/* The hub's count replaced by ours, when ours is the higher one. */
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
	heart_rate = HUB_NONE;
	if (configure(NANOHUB_SENSOR_HEART_RATE, on ? NANOHUB_CMD_ENABLE : NANOHUB_CMD_DISABLE, NANOHUB_RATE_ON_CHANGE, 0))
		hr_on = on;
	else
		log_printf("hub: heart rate %s not taken\n", on ? "on" : "off");
}

static uint32_t nanohub_heart_rate(uint8_t *status, unsigned *readings)
{
	if (status)
		*status = 0;
	if (readings)
		*readings = heart_rate != HUB_NONE;
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

static void nanohub_accel_on(bool on)
{
	want_accel = on;
	have_accel = false;
	configure(NANOHUB_SENSOR_ACCEL, on ? NANOHUB_CMD_ENABLE : NANOHUB_CMD_DISABLE, NANOHUB_HZ(10), 0);
}

static bool nanohub_accel(int16_t v[3])
{
	if (have_accel)
		memcpy(v, accel_v, sizeof accel_v);
	return have_accel;
}

/* Not with events waiting: in WFI only a new edge would wake the CPU. */
static bool nanohub_can_sleep(void)
{
	return !present || (!data_irq && !mode_irq && !hub_irq_low());
}

/* With the screen off the hub draws the time on its own LCD. */
static void nanohub_screen(bool on)
{
	if (started)
		set_mode(on ? MODE_NORMAL : MODE_AMBIENT);
}

/* What the vendor's Essential mode leaves the hub in: its sensors for the
 * watch off, the idle workout frame, then BAND, in which it shows the
 * time, date, battery and steps until the next boot. */

static void nanohub_timepiece(void)
{
	if (!started)
		return;
	want_tilt = want_hr = want_accel = false;
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
	say("hub: nanohub hw %08lx fw %08lx, %s, mode %u, %lu events, %lu errors, steps %ld, gauge %s %lu mV %lu%%\n",
	    versions[0], versions[1], present ? "up" : "not up", mode, events, errors, (long)steps_today,
	    have_gauge ? "" : "none", gauge.mv, gauge.percent);
}

const struct hub_driver hub_nanohub = {
	.init = nanohub_init,
	.poll = nanohub_poll,
	.set_time = set_time,
	.steps_today = nanohub_steps_today,
	.sync_steps_today = nanohub_sync_steps_today,
	.heart_rate_on = nanohub_heart_rate_on,
	.heart_rate = nanohub_heart_rate,
	.tilted = nanohub_tilted,
	.tilt_on = nanohub_tilt_on,
	.accel_on = nanohub_accel_on,
	.accel = nanohub_accel,
	.can_sleep = nanohub_can_sleep,
	.screen = nanohub_screen,
	.status = nanohub_status,
	.gauge = get_gauge,
	.poll_gauge = poll_gauge,
	.timepiece = nanohub_timepiece,
	.workout = nanohub_workout,
	.heart_log = nanohub_heart_log,
	.mode = nanohub_mode,
};
