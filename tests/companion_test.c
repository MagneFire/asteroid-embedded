/* The framing of beluga's AP link: the AP's side of it is played here
 * as deamonserver does it, its writes and its reads. Then the link and
 * the work modes run against a played AP, with the hardware and the
 * rest of the firmware stubbed out. */
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "companion/wire.h"
#include "companion/proto.h"
#include "companion/link.h"
#include "companion/companion.h"
#include "companion/sensors.h"
#include "board/board.h"
#include "sensors/ak09918.h"
#include "sensors/compass.h"
#include "sensors/hub.h"
#include "sensors/lsm6dso.h"
#include "sensors/tsl258x.h"
#include "sched.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* deamonserver's packet: the header, then 112 payload bytes in the first
 * 120-byte part and 120 in each after it; it writes 121 bytes a part. */
static unsigned ap_write_parts(const uint8_t *payload, unsigned len, uint16_t seq, uint8_t parts[][121])
{
	uint8_t frame[WIRE_FRAME_MAX];
	wire_header_put(frame, seq, 0, payload, len);
	memcpy(frame + WIRE_HEADER, payload, len);
	unsigned n = (len + 7) / 120 + 1;
	for (unsigned i = 0; i < n; i++) {
		memset(parts[i], 0, 120);
		parts[i][120] = 0xa5;
		unsigned off = i * 120, k = WIRE_HEADER + len - off < 120 ? WIRE_HEADER + len - off : 120;
		memcpy(parts[i], frame + off, k);
	}
	return n;
}

/* What the AP's 256-byte read sees after the MCU staged a part: the
 * direct area first, its FIFO bytes from 126, and what it keeps of it. */
static void ap_read(const uint8_t *direct, unsigned dn, const uint8_t *fifo, unsigned fn, uint8_t *kept)
{
	uint8_t buf[256];
	memset(buf, 0xee, sizeof buf);
	memcpy(buf, direct, dn);
	memcpy(buf + 126, fifo, fn);
	memcpy(kept, buf, 120);
	memcpy(kept + 120, buf + 126, 128);
}

static void roundtrip_write(unsigned len)
{
	uint8_t payload[WIRE_PAYLOAD_MAX], parts[5][121];
	for (unsigned i = 0; i < len; i++)
		payload[i] = (uint8_t)(i * 7 + len);
	unsigned n = ap_write_parts(payload, len, 9, parts);
	CHECK(n == wire_write_parts(len));
	struct wire_rx rx;
	wire_rx_start(&rx);
	enum wire_error err = WIRE_BAD_SUM;
	for (unsigned i = 0; i < n; i++)
		CHECK(wire_rx_part(&rx, parts[i], &err) == (i + 1 < n ? WIRE_RX_MORE : WIRE_RX_DONE));
	CHECK(err == WIRE_OK && rx.h.len == len && rx.h.seq == 9);
	CHECK(!memcmp(rx.buf + WIRE_HEADER, payload, len));
}

static void roundtrip_read(unsigned len)
{
	uint8_t frame[WIRE_FRAME_MAX], got[5 * WIRE_READ_PART];
	for (unsigned i = 0; i < len; i++)
		frame[WIRE_HEADER + i] = (uint8_t)(i * 13 + 1);
	wire_header_put(frame, 3, WIRE_FLAG_RESYNC, frame + WIRE_HEADER, len);
	unsigned total = WIRE_HEADER + len, parts = wire_read_parts(len);
	/* deamonserver's count, from the length in the first part. */
	CHECK(parts == (len + 7) / 248 + 1);
	for (unsigned p = 0; p < parts; p++) {
		unsigned off, dn, fn;
		wire_read_slice(total, p, &off, &dn, &fn);
		CHECK(off == p * WIRE_READ_PART && dn <= 120 && fn <= 128);
		CHECK(fn == 0 || dn == 120);
		ap_read(frame + off, dn, frame + off + dn, fn, got + p * WIRE_READ_PART);
	}
	CHECK(!memcmp(got, frame, total));
	struct wire_header h;
	CHECK(wire_header_get(got, &h) == WIRE_OK && h.len == len && h.seq == 3 && h.flags == WIRE_FLAG_RESYNC);
	CHECK(wire_sum(got + WIRE_HEADER, h.len) == h.sum);
}

/* The AsteroidOS backend's parse of 377: varints, one nested time with
 * the time in field 2 and the zone in field 3. */
struct ap_reply { int mode, last_mode, reboot; int64_t time; int zone; };

static uint64_t ap_varint(const uint8_t *p, unsigned n, unsigned *i)
{
	uint64_t v = 0;
	for (unsigned shift = 0; *i < n && shift < 64; shift += 7) {
		uint8_t b = p[(*i)++];
		v |= (uint64_t)(b & 0x7f) << shift;
		if (!(b & 0x80))
			break;
	}
	return v;
}

static void ap_parse(const uint8_t *p, unsigned n, struct ap_reply *r)
{
	memset(r, 0, sizeof *r);
	for (unsigned i = 0; i < n;) {
		uint64_t key = ap_varint(p, n, &i), v = ap_varint(p, n, &i);
		if ((key & 7) == 2) {
			if ((key >> 3) == 4)
				for (unsigned j = i; j < i + v;) {
					uint64_t k = ap_varint(p, i + v, &j), w = ap_varint(p, i + v, &j);
					if ((k >> 3) == 2)
						r->time = (int32_t)w;
					else if ((k >> 3) == 3)
						r->zone = (int32_t)((uint32_t)w >> 1 ^ -((uint32_t)w & 1));
				}
			i += v;
		} else if ((key >> 3) == 1)
			r->mode = (int)v;
		else if ((key >> 3) == 3)
			r->last_mode = (int)v;
		else if ((key >> 3) == 5)
			r->reboot = (int)v;
	}
}

#define STORE_WAIT_MS 13000
#define ESCAPE_TEST_MS 6000
enum { UP_INT = 29, DN_INT = 30, DN_ACK = 31, UP_ACK = 33, PINS = 64 };
#define OWN 36
#define MOTOR_EN 26
static const struct board test_board = {
	.ap_link = { UP_INT, UP_ACK, DN_INT, DN_ACK, OWN, 14 },
	.motor = &motor_aw8624, .motor_pins = { .i2c = { .bus = 0, .sda = 6, .scl = 5, .addr = 0x5a }, .enable = MOTOR_EN },
};
const struct board *board = &test_board;

static uint32_t now_ms, extra_us, reset_bits, rtc_writes, rtc_last, kv_sets, wakes, chips_offs;
static unsigned levels[PINS];
static bool irq_on[PINS], outputs[PINS], display_lent, touch_lent, power_lent, key_down, hub_down;
static unsigned i2c_fails;
static void (*irq_fn[PINS])(unsigned);
static void (*ios_fn)(uint32_t);
static uint8_t lram[120], staged[256];
static unsigned staged_n;
static int kv_mode = -1;
static uint8_t kv_mag[64];
static unsigned kv_mag_len;
static int32_t kv_zone;
static uint32_t steps = HUB_NONE, tilts, clock_from_ms, ms_per_read;
static bool accel_shared, hr_shared, tilt_shared;
static uint32_t hr_bpm = HUB_NONE;
static uint8_t hr_quality;
static unsigned hr_readings;
static uint32_t gyro_period, mag_period, gyro_fails, mag_fails, gyro_tries, mag_tries;
static uint32_t light_tries, mag_losts, light_losts;
static int mag_result = AK09918_OK;
static bool light_shared, light_fails;
static int32_t lux = -1;

uint32_t time_ms(void) { return now_ms += ms_per_read; }
uint32_t time_us(void) { return now_ms * 1000 + extra_us; }
uint32_t irq_save(void) { return 0; }
void irq_restore(uint32_t s) {}
bool sched_add(struct sched_task *t) { return true; }
void sched_post(const struct sched_task *t) {}
void gpio_config(unsigned n, unsigned func, unsigned dir, unsigned pull, unsigned drive_ma) { outputs[n] = dir == GPIO_OUTPUT; }
int i2c_init(unsigned bus, unsigned sda, unsigned scl, unsigned func, uint32_t bus_hz) { return 0; }
/* The AW8624 answers with its ID, but for the next i2c_fails transfers. */
int i2c_xfer(unsigned bus, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	if (i2c_fails) {
		i2c_fails--;
		return -1;
	}
	if (rn)
		r[0] = 0x24;
	return 0;
}
unsigned gpio_get(unsigned n) { return levels[n]; }
void gpio_set(unsigned n, unsigned v) { levels[n] = v; }
void delay_ms(uint32_t ms) {}
void gpio_irq_config(unsigned n, unsigned type, void (*fn)(unsigned n)) { irq_fn[n] = fn; }
void gpio_irq_enable(unsigned n, bool on) { irq_on[n] = on; }
void ios_init(void (*fn)(uint32_t status)) { ios_fn = fn; }
uint32_t ios_interrupts(void) { return 0; }
void ios_read(unsigned off, uint8_t *p, unsigned n) { memcpy(p, lram + off, n); }
void ios_stage(const uint8_t *direct, unsigned dn, const uint8_t *fifo, unsigned fn)
{
	memcpy(staged, direct, dn);
	memcpy(staged + dn, fifo, fn);
	staged_n = dn + fn;
}
static const char *kv_take, *kv_fade;
static uint32_t frames_shown, fade_asked;
uint32_t display_frames(void) { return frames_shown; }
void power_fade_in(uint32_t ms) { fade_asked = ms; }
void power_fade_shown(uint32_t now) { (void)now; }
int kv_get(const char *key, void *buf, unsigned size)
{
	if (!strcmp(key, "ap.mode") && kv_mode >= 0) {
		*(uint8_t *)buf = (uint8_t)kv_mode;
		return 1;
	}
	if (!strcmp(key, "ap.take") && kv_take) {
		memcpy(buf, kv_take, strlen(kv_take) < size ? strlen(kv_take) : size);
		return (int)strlen(kv_take);
	}
	if (!strcmp(key, "ap.fade") && kv_fade) {
		memcpy(buf, kv_fade, strlen(kv_fade) < size ? strlen(kv_fade) : size);
		return (int)strlen(kv_fade);
	}
	return -2;
}
int kv_set(const char *key, const void *val, unsigned len)
{
	kv_sets++;
	if (!strcmp(key, "mag.cal") && len <= sizeof kv_mag) {
		memcpy(kv_mag, val, len);
		kv_mag_len = len;
	} else if (!strcmp(key, "ap.mode"))
		kv_mode = *(const uint8_t *)val;
	else if (!strcmp(key, "ap.zone"))
		memcpy(&kv_zone, val, sizeof kv_zone);
	return 0;
}
void log_printf(const char *fmt, ...)
{
	if (!getenv("VERBOSE"))
		return;
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
const char *log_previous_record(void) { return NULL; }
uint32_t reset_status(void) { return reset_bits; }
uint32_t rtc_read(void) { return rtc_last; }
static char noted[64];
void events_note(const char *what) { snprintf(noted, sizeof noted, "%s", what); }
bool rtc_valid(void) { return rtc_writes != 0; }
static bool rtc_ap;
bool rtc_from_ap(void) { return rtc_ap; }
void rtc_write_from_ap(uint32_t secs)
{
	rtc_write(secs);
	rtc_ap = true;
}
void rtc_write(uint32_t secs)
{
	rtc_ap = false;
	rtc_writes++;
	rtc_last = secs;
}
void battery_from_ap(unsigned percent) {}
void hub_set_time(void) {}
void display_lend(bool to_ap) { display_lent = to_ap; }
void display_start_lent(void) { display_lent = true; }
void touch_lend(bool to_ap) { touch_lent = to_ap; }
void touch_start_lent(void) { touch_lent = true; }
void power_lend_display(bool on) { power_lent = on; }
static bool bt_lent;
static unsigned bt_lends;
void action_bluetooth_lend(bool to_ap, uint32_t now)
{
	bt_lent = to_ap;
	bt_lends++;
}
void hub_power_down(bool down) { hub_down = down; }
bool board_chips_off(void)
{
	chips_offs++;
	return true;
}
void iom_stop(unsigned bus) {}
void power_wake(const char *cause) { wakes++; }
bool power_key(void) { return key_down; }
static bool ap_off_ui;
void ui_set_ap_off(bool off) { ap_off_ui = off; }
void hub_accel_share(bool on) { accel_shared = on; }
static int16_t accel_mg[3] = { 0, -1000, 500 };
bool hub_accel(int16_t v[3])
{
	memcpy(v, accel_mg, sizeof accel_mg);
	return accel_shared;
}
void afe4900_share(bool on)
{
	if (on != hr_shared)
		hr_readings = 0, hr_bpm = HUB_NONE;
	hr_shared = on;
}
uint32_t afe4900_rate(uint8_t *status, unsigned *readings)
{
	if (status)
		*status = hr_quality;
	*readings = hr_shared ? hr_readings : 0;
	return hr_bpm;
}
/* The chips take what they are asked but for the next so many tries. */
bool lsm6dso_gyro_on(uint32_t period_ms)
{
	if (period_ms == gyro_period)
		return true;
	gyro_tries++;
	if (gyro_fails && gyro_fails--)
		return false;
	gyro_period = period_ms;
	return true;
}
#define GYRO_RADS	(LSM6DSO_MDPS_PER_COUNT * 0.001f * 3.14159265f / 180)
static int16_t gyro_counts[3] = { 1000, -500, 0 };
bool lsm6dso_gyro(int16_t v[3])
{
	memcpy(v, gyro_counts, sizeof gyro_counts);
	return gyro_period != 0;
}
bool ak09918_on(uint32_t period_ms)
{
	if (period_ms == mag_period)
		return true;
	mag_tries++;
	if (mag_fails && mag_fails--)
		return false;
	mag_period = period_ms;
	return true;
}
void ak09918_lost(void)
{
	mag_losts++;
	mag_period = 0;
}
static int16_t mag_counts[3] = { 100, 200, -300 };

/* The chip's counts for a field on Android's axes, distorted as the
 * compass's correction undoes, plus a hard-iron offset. */
static void set_field(const float f[3], const float iron[3])
{
	float m[3][3], inv[3][3];
	for (int i = 0; i < 3; i++) {
		float e[3] = { i == 0, i == 1, i == 2 }, col[3];
		compass_correct(e, col);
		for (int j = 0; j < 3; j++)
			m[j][i] = col[j];
	}
	float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		    m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			inv[i][j] = (m[(j + 1) % 3][(i + 1) % 3] * m[(j + 2) % 3][(i + 2) % 3] -
				     m[(j + 1) % 3][(i + 2) % 3] * m[(j + 2) % 3][(i + 1) % 3]) /
				    det;
	float raw[3];
	for (int i = 0; i < 3; i++)
		raw[i] = inv[i][0] * f[0] + inv[i][1] * f[1] + inv[i][2] * f[2] + iron[i];
	mag_counts[0] = (int16_t)lroundf(raw[1] / 0.15f);
	mag_counts[1] = (int16_t)lroundf(-raw[0] / 0.15f);
	mag_counts[2] = (int16_t)lroundf(raw[2] / 0.15f);
}
int ak09918_read(int16_t v[3])
{
	memcpy(v, mag_counts, sizeof mag_counts);
	return mag_period ? mag_result : AK09918_NONE;
}
void ak09918_status(void (*say)(const char *fmt, ...)) {}
void tsl258x_share(bool on)
{
	light_tries++;
	light_shared = on;
}
bool tsl258x_settled(void) { return !light_fails; }
void tsl258x_lost(void) { light_losts++; }
int32_t light_finish(void) { return light_shared ? lux : -1; }
uint32_t hub_steps(void) { return steps; }
uint32_t hub_tilts(void) { return tilts; }
void hub_tilt_share(bool on) { tilt_shared = on; }
/* The MCU's clock in µs, which starts over at a reset. */
uint64_t timer_count(void) { return (uint64_t)(now_ms - clock_from_ms) * 1000; }
uint32_t timer_hz(void) { return 1000000; }

static void fire(unsigned pin)
{
	if (irq_on[pin] && irq_fn[pin])
		irq_fn[pin](pin);
}

static uint64_t le(const uint8_t *p, unsigned n)
{
	uint64_t v = 0;
	while (n--)
		v = v << 8 | p[n];
	return v;
}

/* The HAL's decode of a ResponseBody: field 1 a varint, field 2 the
 * repeated SensorsEvtT (1 fixed32 id, 2 fixed64 time, 3 SensorsRawData
 * whose field 1 is SensorsVecT, four fixed32, or 4 SensorsCustData
 * whose field 3 is heart_rate_android_t, a fixed32 rate and a sint32
 * status, both there). Every field is checked for its wire type; -1
 * for anything else. */
struct ap_event { uint32_t id; uint64_t time; uint32_t w[4]; unsigned words; bool heart; float bpm; int32_t status; };

static int ap_heart(const uint8_t *p, unsigned n, struct ap_event *e)
{
	unsigned i = 0;
	if (n < 7 || p[0] != (1 << 3 | 5))
		return -1;
	memcpy(&e->bpm, p + 1, 4);
	i = 5;
	if (ap_varint(p, n, &i) != (2 << 3 | 0))
		return -1;
	uint32_t z = (uint32_t)ap_varint(p, n, &i);
	e->status = (int32_t)(z >> 1 ^ (0u - (z & 1)));
	e->heart = true;
	return i == n ? 0 : -1;
}

static int ap_vec(const uint8_t *p, unsigned n, struct ap_event *e)
{
	for (unsigned i = 0; i < n; i += 5) {
		unsigned k = p[i] >> 3;
		if (i + 5 > n || (p[i] & 7) != 5 || k < 1 || k > 4)
			return -1;
		e->w[k - 1] = (uint32_t)le(p + i + 1, 4);
		e->words++;
	}
	return 0;
}

static int ap_events(const uint8_t *p, unsigned n, struct ap_event *ev, unsigned max, unsigned *field1)
{
	unsigned count = 0;
	*field1 = 0;
	for (unsigned i = 0; i < n;) {
		uint64_t key = ap_varint(p, n, &i), v = ap_varint(p, n, &i);
		if (key == (1 << 3 | 0)) {
			*field1 = (unsigned)v;
			continue;
		}
		if (key != (2 << 3 | 2) || i + v > n || count == max)
			return -1;
		struct ap_event *e = &ev[count++];
		memset(e, 0, sizeof *e);
		const uint8_t *q = p + i;
		unsigned m = (unsigned)v, j = 0;
		i += m;
		if (m < 14 || q[0] != (1 << 3 | 5) || q[5] != (2 << 3 | 1))
			return -1;
		e->id = (uint32_t)le(q + 1, 4);
		e->time = le(q + 6, 8);
		j = 14;
		uint64_t k3 = ap_varint(q, m, &j), l3 = ap_varint(q, m, &j);
		bool cust = k3 == (4 << 3 | 2);
		if ((k3 != (3 << 3 | 2) && !cust) || j + l3 != m)
			return -1;
		const uint8_t *r = q + j;
		unsigned rn = (unsigned)l3, t = 0;
		uint64_t k1 = ap_varint(r, rn, &t), l1 = ap_varint(r, rn, &t);
		if (t + l1 != rn)
			return -1;
		if (cust ? k1 != (3 << 3 | 2) || ap_heart(r + t, (unsigned)l1, e) :
			   k1 != (1 << 3 | 2) || ap_vec(r + t, (unsigned)l1, e))
			return -1;
	}
	return (int)count;
}

/* What the AP has read of the sensor channel so far. */
static struct {
	struct ap_event ev[256];
	unsigned events, reports, most, longest;
	uint64_t last_time;
	bool backwards;
} got;

static void ap_got_report(const struct wire_msg *m, unsigned n)
{
	CHECK(!m->framework && m->chan == WIRE_CHAN_SENSORS);
	struct ap_event e[PROTO_SNS_EVENTS];
	unsigned field1;
	int k = ap_events(m->body, m->len, e, PROTO_SNS_EVENTS, &field1);
	CHECK(k > 0 && field1 == 1);
	for (int j = 0; j < k && got.events < 256; j++) {
		if (e[j].id != PROTO_SNS_META) {
			got.backwards |= e[j].time <= got.last_time;
			got.last_time = e[j].time;
		}
		got.ev[got.events++] = e[j];
	}
	got.reports++;
	got.most = (unsigned)k > got.most ? (unsigned)k : got.most;
	got.longest = n > got.longest ? n : got.longest;
}

static unsigned ap_receive(struct wire_msg *m, uint8_t *copy);

static uint16_t ap_seq = 100;
/* Whether the AP reads what the MCU offers while it waits to write. */
static bool ap_reads = true;

/* dn_int up and held: dn_ack answers once it has been up for a while. */
static void ap_request(void)
{
	levels[DN_INT] = 1;
	fire(DN_INT);
	CHECK(levels[DN_ACK] == 0);
	if (!ms_per_read) {
		CHECK(levels[UP_INT] || companion_next_ms() <= LINK_DN_SETTLE_MS);
		now_ms += LINK_DN_SETTLE_MS - 1;
		link_poll();
		CHECK(levels[DN_ACK] == 0);
	}
	now_ms += ms_per_read ? LINK_DN_SETTLE_MS : 1;
	link_poll();
}

/* deamonserver's write: dn_int up, the parts once dn_ack is up, and
 * dn_ack low again for a good packet. */
static void ap_write_env(const uint8_t *env, unsigned n)
{
	uint8_t parts[5][121];
	unsigned k = ap_write_parts(env, n, ap_seq++, parts);
	ap_request();
	/* The AP reads a sensor report under way first. */
	for (int i = 0; !ap_reads && !levels[DN_ACK] && i < 2000; i++) {
		now_ms++;
		companion_poll();
	}
	while (!levels[DN_ACK] && levels[UP_INT]) {
		struct wire_msg m;
		uint8_t copy[WIRE_PAYLOAD_MAX];
		unsigned n = ap_receive(&m, copy);
		ap_got_report(&m, n);
		link_poll();
	}
	CHECK(levels[DN_ACK] == 1);
	for (unsigned i = 0; i < k; i++) {
		memcpy(lram, parts[i], sizeof lram);
		ios_fn(IOS_WRITTEN);
	}
	CHECK(levels[DN_ACK] == 0);
	levels[DN_INT] = 0;
}

static void ap_write(uint16_t chan, uint16_t id, const uint8_t *body, unsigned len)
{
	uint8_t env[WIRE_PAYLOAD_MAX];
	ap_write_env(env, wire_wrap(env, sizeof env, chan, id, body, len));
}

static void ap_write_sensors(const uint8_t *body, unsigned len)
{
	uint8_t env[WIRE_PAYLOAD_MAX];
	ap_write_env(env, wire_wrap_raw(env, sizeof env, WIRE_CHAN_SENSORS, body, len));
	companion_pass();
}

/* The AP reads what up_int offers and answers with a short pulse. */
static unsigned ap_receive(struct wire_msg *m, uint8_t *copy)
{
	if (!levels[UP_INT])
		return 0;
	struct wire_header h;
	CHECK(wire_header_get(staged, &h) == WIRE_OK && h.len + WIRE_HEADER <= staged_n);
	memcpy(copy, staged + WIRE_HEADER, h.len);
	CHECK(wire_sum(copy, h.len) == h.sum);
	CHECK(wire_unwrap(copy, h.len, m));
	levels[UP_ACK] = 1;
	fire(UP_ACK);
	extra_us += 1000;
	levels[UP_ACK] = 0;
	fire(UP_ACK);
	extra_us = 0;
	CHECK(!levels[UP_INT]);
	return h.len;
}

static bool ap_take(struct wire_msg *m, uint8_t *copy)
{
	if (!ap_receive(m, copy))
		return false;
	CHECK(m->framework);
	return true;
}

static void wait_ms(uint32_t ms)
{
	now_ms += ms;
	companion_pass();
}

/* One pass a millisecond, as when nothing else keeps the loop busy. */
static void run_ms(uint32_t ms)
{
	while (ms--)
		wait_ms(1);
}

/* The mode in the 377 up_int offers, or -1. */
static int staged_mode(void)
{
	struct wire_header h;
	struct wire_msg m;
	struct ap_reply a;
	uint8_t copy[WIRE_PAYLOAD_MAX];
	if (!levels[UP_INT] || wire_header_get(staged, &h) != WIRE_OK)
		return -1;
	memcpy(copy, staged + WIRE_HEADER, h.len);
	if (!wire_unwrap(copy, h.len, &m) || !m.framework || m.id != PROTO_REPLY)
		return -1;
	ap_parse(m.body, m.len, &a);
	return a.mode;
}

/* A work mode message answered and acknowledged. */
static void mode_message(uint16_t id, const uint8_t *body, unsigned len, struct ap_reply *a)
{
	ap_write(1, id, body, len);
	companion_pass();
	struct wire_msg m;
	uint8_t copy[WIRE_PAYLOAD_MAX];
	CHECK(ap_take(&m, copy) && m.id == PROTO_REPLY && m.chan == 1);
	ap_parse(m.body, m.len, a);
	companion_pass();
}

static bool status_has(const char *s);
#define STORED_OFF 3

static void lk_ask(void);
static void lk_done(void);

/* After a 374: nothing taken back, no wake, no Bluetooth and the
 * sensors off, also with the power key held. */
static void powered_off(unsigned lends, unsigned woken)
{
	run_ms(3000);
	CHECK(display_lent && touch_lent && power_lent && !levels[OWN] && !outputs[MOTOR_EN]);
	CHECK(bt_lent && bt_lends == lends && hub_down && wakes == woken);
	lk_ask();
	CHECK(!levels[DN_ACK]);
	wait_ms(80);
	lk_done();
	run_ms(LINK_DN_SETTLE_MS);
	CHECK(!levels[DN_ACK] && !levels[UP_INT] && status_has("boot loader answered 0 times"));
	key_down = true;
	run_ms(ESCAPE_TEST_MS);
	key_down = false;
	wait_ms(1);
	CHECK(display_lent && wakes == woken && status_has("power off mode") && status_has("nobody has the panel"));
}

/* The boot loader's ask: dn_int up with up_ack, its notice, held
 * until the answer has dropped. */
static void lk_ask(void)
{
	levels[UP_ACK] = 1;
	fire(UP_ACK);
	levels[DN_INT] = 1;
	fire(DN_INT);
}

static void lk_done(void)
{
	levels[DN_INT] = levels[UP_ACK] = 0;
	fire(UP_ACK);
}

/* The watch has the panel and the AP boots. */
static void lk_answered(void)
{
	lk_ask();
	CHECK(levels[DN_ACK] && !levels[UP_INT] && !display_lent);
	CHECK(companion_next_ms() <= LINK_LK_ANSWER_MS);
	wait_ms(LINK_LK_ANSWER_MS - 1);
	CHECK(levels[DN_ACK] && !display_lent);
	wait_ms(1);
	CHECK(!levels[DN_ACK]);
	CHECK(display_lent && touch_lent && power_lent && !levels[OWN] && !outputs[MOTOR_EN] && bt_lent);
	CHECK(!ap_off_ui);
	lk_done();
}

static void lk_scenario(void)
{
	static const uint8_t m375[] = { 0x08, 0x01 };
	struct ap_reply a;
	struct wire_msg m;
	uint8_t copy[WIRE_PAYLOAD_MAX];
	CHECK(!display_lent && status_has("wristband mode"));

	/* An ordinary request from the AP waits as before. */
	mode_message(PROTO_MODE_REQUEST, NULL, 0, &a);
	CHECK(a.mode == PROTO_MODE_WRISTBAND && !display_lent && status_has("boot loader answered 0 times"));

	/* No reset after it: the AP keeps the panel and the mode is normal
	 * a second later. */
	lk_answered();
	CHECK(status_has("wristband mode") && companion_next_ms() <= 1000);
	wait_ms(999);
	CHECK(status_has("wristband mode"));
	wait_ms(1);
	CHECK(status_has("normal mode, wristband before") && status_has("1 lent after the boot loader"));
	run_ms(STORE_WAIT_MS);
	CHECK(kv_mode == PROTO_MODE_NORMAL && display_lent);

	/* Not while the AP has the panel. */
	lk_ask();
	CHECK(!levels[DN_ACK]);
	wait_ms(80);
	lk_done();
	run_ms(LINK_DN_SETTLE_MS);
	CHECK(!levels[DN_ACK] && status_has("boot loader answered 1 times"));

	/* Not with a packet for the AP under way either: up_ack is the AP's
	 * answer to it then. */
	ap_write(1, 375, m375, sizeof m375);
	companion_pass();
	run_ms(1000);
	CHECK(!display_lent);
	ap_reads = false;
	ap_write(1, PROTO_MODE_REQUEST, NULL, 0);
	companion_pass();
	CHECK(levels[UP_INT]);
	lk_ask();
	CHECK(!levels[DN_ACK]);
	levels[DN_INT] = levels[UP_ACK] = 0;
	ap_reads = true;
	CHECK(ap_take(&m, copy) && m.id == PROTO_REPLY);
	run_ms(LINK_DN_SETTLE_MS);
	CHECK(!levels[DN_ACK] && !display_lent && status_has("boot loader answered 1 times"));

	/* Lines that drop before the answer does were not the boot loader. */
	lk_ask();
	CHECK(levels[DN_ACK]);
	wait_ms(10);
	lk_done();
	run_ms(LINK_LK_ANSWER_MS);
	CHECK(!levels[DN_ACK] && !display_lent && status_has("1 let go early"));

	/* A packet after the answer is the AP after all, and taken. */
	uint8_t env[16], parts[5][121];
	ap_write_parts(env, wire_wrap(env, sizeof env, 1, PROTO_MODE_REQUEST, NULL, 0), ap_seq++, parts);
	lk_ask();
	CHECK(levels[DN_ACK]);
	wait_ms(5);
	memcpy(lram, parts[0], sizeof lram);
	ios_fn(IOS_WRITTEN);
	CHECK(!levels[DN_ACK]);
	lk_done();
	companion_pass();
	CHECK(ap_take(&m, copy) && m.id == PROTO_REPLY);
	run_ms(LINK_LK_ANSWER_MS);
	CHECK(!display_lent && status_has("1 lent after the boot loader"));

	/* Answered also with dn_int ignored, and the reset that follows
	 * boots lent. */
	run_ms(STORE_WAIT_MS);
	CHECK(kv_mode == PROTO_MODE_WRISTBAND);
	for (int i = 0; i < LINK_UNFINISHED_MAX; i++) {
		ap_request();
		levels[DN_INT] = 0;
		wait_ms(LINK_RX_MS);
	}
	CHECK(status_has("times, now") && irq_on[DN_INT]);
	lk_answered();
	wait_ms(200);
	reset_bits = 1u << 0;
	CHECK(companion_boot());
	companion_init();
	CHECK(display_lent && !levels[OWN] && status_has("normal mode, wristband before"));
	/* A reset would clear the link's counters, the AP's next write does here. */
	run_ms(LINK_DEAF_MS + STORE_WAIT_MS);
	CHECK(kv_mode == PROTO_MODE_NORMAL && !status_has("times, now"));
	mode_message(PROTO_MODE_REQUEST, NULL, 0, &a);
	CHECK(a.mode == PROTO_MODE_NORMAL && a.last_mode == PROTO_MODE_WRISTBAND && a.reboot == PROTO_REBOOT_PIN);
}

static void companion_scenario(void)
{
	static const uint8_t m375[] = { 0x08, 0x01, 0x12, 0x09, 0x08, 0x80, 0xe2, 0xcf, 0xaa, 0x06, 0x18, 0xd7, 0x04 };
	static const uint8_t m374[] = { 0x08, 0x02 };
	struct ap_reply a;
	struct wire_msg m;
	uint8_t copy[WIRE_PAYLOAD_MAX];
	now_ms = 1000;

	/* Reset by the debugger in the normal mode: the AP keeps the panel. */
	kv_mode = PROTO_MODE_NORMAL;
	reset_bits = 1u << 5;
	levels[OWN] = 1;
	CHECK(companion_boot());
	CHECK(display_lent && touch_lent && !outputs[MOTOR_EN] && !levels[OWN] && !chips_offs);
	CHECK(haptics_init() && !outputs[MOTOR_EN]);
	companion_init();
	CHECK(power_lent && irq_on[DN_INT] && bt_lent);

	/* The AP's boot loader strobes dn_int for 80 ms and takes any answer
	 * as a boot state: none comes, and a 377 waiting goes after it. */
	ap_write(1, PROTO_MODE_REQUEST, NULL, 0);
	levels[DN_INT] = 1;
	fire(DN_INT);
	for (int i = 0; i < 16; i++) {
		wait_ms(5);
		CHECK(!levels[DN_ACK] && !levels[UP_INT]);
	}
	levels[DN_INT] = 0;
	wait_ms(1);
	CHECK(!levels[DN_ACK] && levels[UP_INT]);
	CHECK(ap_take(&m, copy) && m.id == PROTO_REPLY);
	companion_pass();

	/* A truncated 375 changes nothing and gets no 377. */
	ap_write(1, 375, m375, sizeof m375 - 1);
	companion_pass();
	CHECK(!levels[UP_INT] && !rtc_writes && display_lent);
	wait_ms(5000);
	CHECK(!levels[UP_INT] && !kv_sets && display_lent);

	/* A 373 without a time, on a channel past the framework's: an empty
	 * 377 on channel 1. */
	ap_write(7, 373, NULL, 0);
	companion_pass();
	CHECK(ap_take(&m, copy) && m.id == PROTO_REPLY && m.chan == 1 && m.len == 0);
	companion_pass();
	CHECK(display_lent && !kv_sets);

	/* 375: no 377, as the AP lets go of the panel without waiting for
	 * one; the panel comes back 300 ms later, the store waits for the
	 * end of the boot burst and a quiet link. */
	ms_per_read = 1;
	ap_write(1, 375, m375, sizeof m375);
	companion_pass();
	ms_per_read = 0;
	CHECK(!levels[UP_INT] && rtc_writes == 1);
	CHECK(display_lent && power_lent);
	uint32_t left = companion_next_ms();
	CHECK(left > 200 && left <= 300);
	wait_ms(left - 1);
	CHECK(display_lent && !ap_off_ui);
	wait_ms(1);
	CHECK(!display_lent && !touch_lent && !power_lent && wakes == 1 && !kv_sets && levels[OWN]);
	/* The picture fades in, and its first frame is timed for the
	 * history. */
	CHECK(fade_asked == 400 && !strstr(noted, "face shown"));
	wait_ms(120);
	CHECK(!strstr(noted, "face shown"));
	frames_shown++;
	wait_ms(5);
	CHECK(!strcmp(noted, "face shown 125 ms after the take, asked 300 ms before it"));
	/* Only the AP's own key starts it now, and the UI says so. */
	CHECK(ap_off_ui);
	CHECK(!bt_lent);
	CHECK(outputs[MOTOR_EN] && levels[MOTOR_EN]);
	/* The chips on the AP's bus are put to sleep once it is the watch's,
	 * and their drivers start over. */
	CHECK(chips_offs == 1 && mag_losts == 1 && light_losts == 1);
	ap_write(1, PROTO_OOBE, NULL, 0);
	companion_pass();
	wait_ms(2999);
	CHECK(!kv_sets);
	wait_ms(1);
	CHECK(kv_sets == 2 && kv_mode == PROTO_MODE_WRISTBAND && kv_zone == -300);

	/* 376 asks for the mode and changes nothing. */
	mode_message(PROTO_MODE_REQUEST, NULL, 0, &a);
	CHECK(a.mode == PROTO_MODE_WRISTBAND && a.last_mode == PROTO_MODE_NORMAL && a.reboot == PROTO_REBOOT_DEBUG);
	wait_ms(5000);
	CHECK(!display_lent && kv_sets == 2);

	/* The AP's time replaces the AP's, but not a time the phone set;
	 * the AP's zone is used either way. */
	static const uint8_t b386[] = { 0x08, 0x02, 0x12, 0x02, 0x08, 0x01, 0x1a, 0x09,
					0x08, 0x80, 0xe2, 0xcf, 0xaa, 0x06, 0x18, 0xd7, 0x04 };
	CHECK(rtc_ap && rtc_last == 1700000000 - 300 * 60);
	rtc_last += 3600;
	ap_write(1, PROTO_BOOT, b386, sizeof b386);
	companion_pass();
	CHECK(rtc_ap && rtc_last == 1700000000 - 300 * 60 && rtc_writes == 2);
	rtc_write(1800000000);
	ap_write(1, PROTO_BOOT, b386, sizeof b386);
	companion_pass();
	CHECK(!rtc_ap && rtc_last == 1800000000 && rtc_writes == 3);
	mode_message(PROTO_MODE_REQUEST, NULL, 0, &a);
	CHECK(a.time == 1800000000 + 300 * 60 && a.zone == -300);
	wait_ms(5000);
	CHECK(kv_sets == 2);

	/* A 377 the AP never reads: no panel for the AP, no mode stored. */
	ap_write(1, 373, NULL, 0);
	companion_pass();
	CHECK(levels[UP_INT]);
	wait_ms(LINK_TX_MS);
	CHECK(!levels[UP_INT]);
	wait_ms(150);
	CHECK(levels[UP_INT]);
	wait_ms(LINK_TX_MS);
	CHECK(!levels[UP_INT]);
	unsigned lends = bt_lends;
	wait_ms(11000);
	wait_ms(1);
	CHECK(!display_lent && !power_lent && kv_sets == 2 && !bt_lent && bt_lends == lends);

	/* 373, then 374: nobody takes the panel back. */
	mode_message(373, NULL, 0, &a);
	CHECK(a.mode == PROTO_MODE_NORMAL && a.last_mode == PROTO_MODE_WRISTBAND);
	CHECK(display_lent && touch_lent && !outputs[MOTOR_EN] && power_lent && !levels[OWN] && bt_lent);
	CHECK(!ap_off_ui);
	/* A reset before the store has the normal mode keeps the AP's
	 * panel. */
	CHECK(kv_mode == PROTO_MODE_WRISTBAND);
	reset_bits = 1u << 0;
	CHECK(companion_boot() && display_lent && !levels[OWN]);
	lends = bt_lends;
	mode_message(374, m374, sizeof m374, &a);
	CHECK(a.mode == PROTO_MODE_POWER_OFF && a.last_mode == PROTO_MODE_NORMAL && display_lent);
	powered_off(lends, 1);
	run_ms(STORE_WAIT_MS);
	CHECK(kv_mode == STORED_OFF);

	/* A reset without the AP's pin keeps it off, also a power-on. */
	static const uint32_t not_pin[] = { 1u << 1, 1u << 2, 1u << 3, 1u << 6 };
	for (unsigned i = 0; i < sizeof not_pin / sizeof not_pin[0]; i++) {
		reset_bits = not_pin[i];
		hub_down = bt_lent = display_lent = touch_lent = power_lent = false;
		levels[OWN] = 1;
		CHECK(companion_boot() && display_lent && touch_lent && !levels[OWN] && !outputs[MOTOR_EN]);
		companion_init();
		CHECK(power_lent && bt_lent && hub_down && status_has("power off mode") && status_has("nobody has the panel"));
		powered_off(bt_lends, 1);
	}

	/* A 373 brings it back lent. */
	mode_message(373, NULL, 0, &a);
	CHECK(a.mode == PROTO_MODE_NORMAL && a.last_mode == PROTO_MODE_POWER_OFF);
	CHECK(display_lent && power_lent && bt_lent && !hub_down && !levels[OWN]);
	run_ms(STORE_WAIT_MS);
	CHECK(kv_mode == PROTO_MODE_NORMAL);

	/* The power key held for 5 s takes the panel back from an AP that
	 * does not answer, and power off is stored at once. */
	mode_message(373, NULL, 0, &a);
	CHECK(display_lent);
	unsigned sets = kv_sets;
	key_down = true;
	wait_ms(10);
	CHECK(companion_next_ms() <= 5000);
	wait_ms(4999);
	CHECK(display_lent);
	wait_ms(1);
	CHECK(!display_lent && wakes == 2 && kv_sets == sets + 1 && kv_mode == PROTO_MODE_POWER_OFF && !bt_lent);
	/* On its own the watch keeps its Reboot. */
	CHECK(!ap_off_ui);
	key_down = false;

	/* A 374 whose 377 the AP never reads, as it is on its way down:
	 * power off all the same. */
	mode_message(373, NULL, 0, &a);
	CHECK(display_lent && !levels[OWN]);
	lends = bt_lends;
	ap_write(1, 374, m374, sizeof m374);
	companion_pass();
	CHECK(levels[UP_INT] && display_lent);
	powered_off(lends, 2);
	run_ms(2 * LINK_TX_MS + 150);
	CHECK(!levels[UP_INT]);
	run_ms(STORE_WAIT_MS);
	CHECK(kv_mode == STORED_OFF);

	/* A 376 crossing a 373 whose 377 the AP never reads: the 377 for
	 * the 376 does not make the normal mode take effect. */
	ap_reads = false;
	ap_write(1, 373, NULL, 0);
	companion_pass();
	CHECK(staged_mode() == PROTO_MODE_NORMAL);
	ap_write(1, PROTO_MODE_REQUEST, NULL, 0);
	ap_reads = true;
	for (int i = 0; i < 2 * LINK_TX_MS && staged_mode() != PROTO_MODE_POWER_OFF; i++)
		wait_ms(1);
	CHECK(ap_take(&m, copy) && m.id == PROTO_REPLY);
	ap_parse(m.body, m.len, &a);
	CHECK(a.mode == PROTO_MODE_POWER_OFF);
	run_ms(1000);
	CHECK(display_lent && power_lent && !levels[OWN]);

	/* A 375 crossing a 373 whose 377 the AP reads late: the 375 wins. */
	ap_reads = false;
	ap_write(1, 373, NULL, 0);
	companion_pass();
	CHECK(staged_mode() == PROTO_MODE_NORMAL);
	ap_write(1, 375, m375, sizeof m375);
	companion_pass();
	ap_reads = true;
	for (int i = 0; i < LINK_TX_MS && !levels[UP_INT]; i++)
		wait_ms(1);
	CHECK(staged_mode() == PROTO_MODE_NORMAL && ap_take(&m, copy));
	run_ms(1000);
	CHECK(!display_lent && !power_lent);
	mode_message(PROTO_MODE_REQUEST, NULL, 0, &a);
	CHECK(a.mode == PROTO_MODE_WRISTBAND && a.last_mode == PROTO_MODE_POWER_OFF);
	run_ms(STORE_WAIT_MS);

	/* The AW8624 not answering when the panel comes back: its enable is
	 * let go of, and it is tried again the next time. */
	mode_message(373, NULL, 0, &a);
	CHECK(display_lent && !outputs[MOTOR_EN]);
	i2c_fails = 1;
	ap_write(1, 375, m375, sizeof m375);
	companion_pass();
	run_ms(1000);
	CHECK(!display_lent && !i2c_fails && !outputs[MOTOR_EN]);
	mode_message(373, NULL, 0, &a);
	CHECK(display_lent && !outputs[MOTOR_EN]);
	ap_write(1, 375, m375, sizeof m375);
	companion_pass();
	run_ms(1000);
	CHECK(!display_lent && outputs[MOTOR_EN] && levels[MOTOR_EN]);
	run_ms(STORE_WAIT_MS);

	/* The store's times for the take and the fade, and the shortest
	 * take for one too short; text that is no number changes nothing. */
	static const struct { const char *take, *fade; uint32_t wait, faded; } stored[] = {
		{ "300", "0", 300, 0 }, { "5", "9999", 200, 2000 }, { "1s", "", 300, 400 }, { "123456789", "x", 300, 400 },
	};
	mode_message(373, NULL, 0, &a);
	for (unsigned i = 0; i < sizeof stored / sizeof stored[0]; i++) {
		CHECK(display_lent);
		kv_take = stored[i].take;
		kv_fade = stored[i].fade;
		ap_write(1, 375, m375, sizeof m375);
		companion_pass();
		CHECK(companion_next_ms() <= stored[i].wait);
		run_ms(stored[i].wait - 1);
		CHECK(display_lent);
		run_ms(1);
		CHECK(!display_lent && fade_asked == stored[i].faded);
		/* Lent again before a frame: nothing is timed. */
		noted[0] = 0;
		mode_message(373, NULL, 0, &a);
		frames_shown++;
		run_ms(2);
		CHECK(!strstr(noted, "face shown"));
	}
	kv_take = kv_fade = NULL;
	ap_write(1, 375, m375, sizeof m375);
	companion_pass();
	run_ms(1000);
	CHECK(!display_lent);
	run_ms(STORE_WAIT_MS);

	lk_scenario();

	/* Requests without a packet: dn_int is ignored after three, until
	 * the AP writes, or for a while. */
	for (int i = 0; i < LINK_UNFINISHED_MAX; i++) {
		ap_request();
		CHECK(levels[DN_ACK] == 1);
		/* The deadline a light sleep wakes for. */
		CHECK(link_next_ms() == LINK_RX_MS);
		levels[DN_INT] = 0;
		wait_ms(LINK_RX_MS);
		CHECK(levels[DN_ACK] == 0);
	}
	CHECK(!irq_on[DN_INT]);
	ios_fn(IOS_WRITTEN);
	CHECK(irq_on[DN_INT]);
	for (int i = 0; i < LINK_UNFINISHED_MAX; i++) {
		ap_request();
		levels[DN_INT] = 0;
		wait_ms(LINK_RX_MS);
	}
	CHECK(!irq_on[DN_INT]);
	/* An AP that waits for dn_ack before it writes gets it once the
	 * wait is over. */
	levels[DN_INT] = 1;
	fire(DN_INT);
	CHECK(companion_next_ms() <= LINK_DEAF_MS);
	wait_ms(LINK_DEAF_MS - 1);
	CHECK(!irq_on[DN_INT] && !levels[DN_ACK]);
	wait_ms(1);
	CHECK(irq_on[DN_INT] && !levels[DN_ACK]);
	wait_ms(LINK_DN_SETTLE_MS);
	CHECK(levels[DN_ACK] == 1);
	uint8_t env[16], parts[5][121];
	ap_write_parts(env, wire_wrap(env, sizeof env, 1, PROTO_OOBE, NULL, 0), ap_seq++, parts);
	memcpy(lram, parts[0], sizeof lram);
	ios_fn(IOS_WRITTEN);
	CHECK(levels[DN_ACK] == 0);
	levels[DN_INT] = 0;

	/* Nothing over what one read's direct bytes carry. */
	uint8_t big[LINK_SEND_MAX + 1] = { 0 };
	CHECK(!link_send(big, sizeof big));
	CHECK(link_send(big, LINK_SEND_MAX));
}

static float bits_float(uint32_t u)
{
	float f;
	memcpy(&f, &u, sizeof f);
	return f;
}

static void sensor_tests(void)
{
	/* The HAL's commands, from the spec's templates. */
	static const uint8_t enable[] = { 0x08, 0x03, 0x2a, 0x10, 0x0d, 0x01, 0x00, 0x00, 0x00, 0x10, 0x01,
					  0x19, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x00 };
	struct proto_sns_cmd c;
	CHECK(proto_sns_cmd(enable, sizeof enable, &c) && c.op == PROTO_SNS_ACTIVATE && c.handle == 1 && c.enable &&
	      c.time_ns == 0x0066554433221100ull);
	static const uint8_t disable[] = { 0x08, 0x03, 0x2a, 0x10, 0x0d, 0x13, 0x00, 0x00, 0x00, 0x10, 0x00,
					   0x19, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	CHECK(proto_sns_cmd(disable, sizeof disable, &c) && c.op == PROTO_SNS_ACTIVATE && c.handle == 19 &&
	      !c.enable && c.time_ns == 1);
	/* 20 ms (0x1312d00 ns) a sample, 1 s (0x3b9aca00 ns) of latency. */
	static const uint8_t batch[] = { 0x08, 0x07, 0x22, 0x17, 0x0d, 0x1a, 0x00, 0x00, 0x00,
					 0x11, 0x00, 0x2d, 0x31, 0x01, 0x00, 0x00, 0x00, 0x00,
					 0x19, 0x00, 0xca, 0x9a, 0x3b, 0x00, 0x00, 0x00, 0x00 };
	CHECK(proto_sns_cmd(batch, sizeof batch, &c) && c.op == PROTO_SNS_BATCH && c.handle == 26 &&
	      c.period_ns == 20000000 && c.latency_ns == 1000000000 && !c.time_ns);
	static const uint8_t flush[] = { 0x08, 0x02, 0x1a, 0x05, 0x0d, 0x24, 0x00, 0x01, 0x00 };
	CHECK(proto_sns_cmd(flush, sizeof flush, &c) && c.op == PROTO_SNS_FLUSH && c.handle == 65572);
	CHECK(!proto_sns_cmd(flush, sizeof flush - 1, &c));
	static const uint8_t no_payload[] = { 0x08, 0x02 };
	CHECK(!proto_sns_cmd(no_payload, sizeof no_payload, &c));

	/* A report of an accelerometer sample, a step count and a flush. */
	struct proto_sns_event e[3] = {
		{ PROTO_SNS_ACCEL, 0x0102030405060708ull,
		  { proto_float_bits(0.5f), proto_float_bits(-9.80665f), proto_float_bits(1.25f), 3 } },
		{ PROTO_SNS_STEP_COUNTER, 5000000000ull, { proto_float_bits(1234.0f), 0, 0, 0 } },
		{ PROTO_SNS_META, 7, { PROTO_SNS_ACCEL, 0, 0, 0 } },
	};
	uint8_t out[PROTO_SNS_REPORT_MAX];
	unsigned n = proto_sns_report(out, sizeof out, e, 3);
	CHECK(n == 2 + 3 * PROTO_SNS_EVENT_LEN);
	static const uint8_t head[] = { 0x08, 0x01, 0x12, 0x26, 0x0d, 0x01, 0x00, 0x00, 0x00,
					0x11, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
					0x1a, 0x16, 0x0a, 0x14, 0x0d, 0x00, 0x00, 0x00, 0x3f, 0x15 };
	CHECK(!memcmp(out, head, sizeof head));
	struct ap_event a[PROTO_SNS_EVENTS];
	unsigned field1;
	CHECK(ap_events(out, n, a, PROTO_SNS_EVENTS, &field1) == 3 && field1 == 1);
	CHECK(a[0].id == 1 && a[0].time == 0x0102030405060708ull && a[0].words == 4);
	CHECK(bits_float(a[0].w[0]) == 0.5f && bits_float(a[0].w[1]) == -9.80665f && bits_float(a[0].w[2]) == 1.25f &&
	      a[0].w[3] == 3);
	CHECK(a[1].id == 19 && a[1].time == 5000000000ull && (uint64_t)bits_float(a[1].w[0]) == 1234);
	CHECK(a[2].id == 0 && a[2].w[0] == 1 && a[2].words == 4);

	/* A heart rate as the HAL's heart_rate_android_t, the status
	 * zigzagged and written also when it is 0. */
	struct proto_sns_event hr[3] = {
		{ PROTO_SNS_HEART_RATE, 9, { proto_float_bits(72.0f), PROTO_SNS_HIGH, 0, 0 } },
		{ PROTO_SNS_HEART_RATE, 10, { 0, (uint32_t)PROTO_SNS_NO_CONTACT, 0, 0 } },
		{ PROTO_SNS_HEART_RATE, 11, { proto_float_bits(55.0f), PROTO_SNS_UNRELIABLE, 0, 0 } },
	};
	n = proto_sns_report(out, sizeof out, hr, 3);
	static const uint8_t hr_event[] = { 0x12, 0x19, 0x0d, 0x15, 0x00, 0x00, 0x00, 0x11, 0x09, 0, 0, 0, 0, 0, 0, 0,
					    0x22, 0x09, 0x1a, 0x07, 0x0d, 0x00, 0x00, 0x90, 0x42, 0x10, 0x06 };
	CHECK(n == 2 + 3 * 27 && !memcmp(out + 2, hr_event, sizeof hr_event));
	CHECK(ap_events(out, n, a, PROTO_SNS_EVENTS, &field1) == 3);
	CHECK(a[0].id == 21 && a[0].heart && a[0].bpm == 72.0f && a[0].status == 3 && a[0].time == 9);
	CHECK(a[1].heart && a[1].bpm == 0 && a[1].status == -1);
	CHECK(a[2].heart && a[2].bpm == 55.0f && a[2].status == 0);

	/* Nine events fit, in what the HAL reads at once with the envelope. */
	struct proto_sns_event nine[PROTO_SNS_EVENTS];
	for (unsigned i = 0; i < PROTO_SNS_EVENTS; i++)
		nine[i] = e[0];
	n = proto_sns_report(out, sizeof out, nine, PROTO_SNS_EVENTS);
	CHECK(n == PROTO_SNS_REPORT_MAX && n + 4 <= 512 && n + 4 <= WIRE_PAYLOAD_MAX);
	CHECK(ap_events(out, n, a, PROTO_SNS_EVENTS, &field1) == PROTO_SNS_EVENTS);
	CHECK(proto_sns_report(out, sizeof out - 1, nine, PROTO_SNS_EVENTS) == 0);

	/* On channel 4 the body follows the channel and its length. */
	uint8_t env[4 + PROTO_SNS_REPORT_MAX];
	unsigned k = wire_wrap_raw(env, sizeof env, WIRE_CHAN_SENSORS, out, n);
	struct wire_msg m;
	CHECK(k == 4 + n && env[0] == 4 && env[1] == 0 && env[2] == (uint8_t)n && env[3] == n >> 8);
	CHECK(wire_unwrap(env, k, &m) && !m.framework && m.chan == 4 && m.len == n && !memcmp(m.body, out, n));
	CHECK(wire_wrap_raw(env, n + 3, WIRE_CHAN_SENSORS, out, n) == 0);
}

static unsigned put_le(uint8_t *p, uint64_t v, unsigned n)
{
	for (unsigned i = 0; i < n; i++, v >>= 8)
		p[i] = (uint8_t)v;
	return n;
}

/* The HAL's commands, as in sensor_tests. */
static void ap_activate(uint32_t handle, bool on, uint64_t time_ns)
{
	uint8_t b[20] = { 0x08, 0x03, 0x2a, 0x10, 0x0d };
	put_le(b + 5, handle, 4);
	b[9] = 0x10, b[10] = on, b[11] = 0x19;
	put_le(b + 12, time_ns, 8);
	ap_write_sensors(b, sizeof b);
}

static void ap_batch(uint32_t handle, uint64_t period_ns, uint64_t latency_ns)
{
	uint8_t b[27] = { 0x08, 0x07, 0x22, 0x17, 0x0d };
	put_le(b + 5, handle, 4);
	b[9] = 0x11;
	put_le(b + 10, period_ns, 8);
	b[18] = 0x19;
	put_le(b + 19, latency_ns, 8);
	ap_write_sensors(b, sizeof b);
}

static void ap_flush(uint32_t handle)
{
	uint8_t b[9] = { 0x08, 0x02, 0x1a, 0x05, 0x0d };
	put_le(b + 5, handle, 4);
	ap_write_sensors(b, sizeof b);
}

/* Reads each report as it comes, for ms milliseconds. */
static void ap_sensors(uint32_t ms)
{
	for (uint32_t i = 0; i < ms; i++) {
		wait_ms(1);
		struct wire_msg m;
		uint8_t copy[WIRE_PAYLOAD_MAX];
		unsigned n = ap_receive(&m, copy);
		if (n)
			ap_got_report(&m, n);
	}
}

static void sensor_scenario(void)
{
	struct ap_reply a;
	/* The OOBE and the empty payload the last scenario left. */
	companion_pass();
	CHECK(levels[UP_INT]);
	levels[UP_ACK] = 1;
	fire(UP_ACK);
	levels[UP_ACK] = 0;
	fire(UP_ACK);
	companion_pass();

	/* The most events in what link_send takes, from the encoder. */
	struct proto_sns_event e[PROTO_SNS_EVENTS] = { 0 };
	uint8_t body[PROTO_SNS_REPORT_MAX];
	unsigned fit = 0;
	while (fit < PROTO_SNS_EVENTS && 4 + proto_sns_report(body, sizeof body, e, fit + 1) <= LINK_SEND_MAX)
		fit++;
	CHECK(fit >= 1 && fit < PROTO_SNS_EVENTS);

	/* 50 Hz asked with a second of latency: the hub's 26 Hz, and the
	 * full queue of nine goes out in reports that each fit. */
	ap_batch(PROTO_SNS_ACCEL, 20000000, 1000000000);
	ap_activate(PROTO_SNS_ACCEL, true, 5000000000ull);
	CHECK(accel_shared);
	memset(&got, 0, sizeof got);
	ap_sensors(8 * 39 - 10);
	CHECK(got.reports == 0);
	ap_sensors(20);
	CHECK(got.events == PROTO_SNS_EVENTS && got.reports == (PROTO_SNS_EVENTS + fit - 1) / fit);
	CHECK(got.most == fit && got.longest <= LINK_SEND_MAX);
	CHECK(got.ev[0].id == PROTO_SNS_ACCEL && got.ev[0].time >= 5000000000ull && got.ev[0].words == 4);
	CHECK(bits_float(got.ev[0].w[1]) == -1000 * 0.00980665f && bits_float(got.ev[0].w[2]) == 500 * 0.00980665f);
	CHECK(got.ev[1].time - got.ev[0].time == 39000000 && !got.backwards);

	/* A flush is answered after the events queued before it. */
	memset(&got, 0, sizeof got);
	ap_sensors(100);
	CHECK(got.reports == 0);
	ap_flush(PROTO_SNS_ACCEL);
	ap_sensors(5);
	CHECK(got.events >= 3 && got.longest <= LINK_SEND_MAX);
	CHECK(got.ev[got.events - 1].id == PROTO_SNS_META && got.ev[got.events - 1].w[0] == PROTO_SNS_ACCEL);
	for (unsigned i = 0; i + 1 < got.events; i++)
		CHECK(got.ev[i].id == PROTO_SNS_ACCEL);

	/* A flush whose report the AP does not read is answered in the
	 * next one. */
	ap_flush(PROTO_SNS_ACCEL);
	CHECK(levels[UP_INT]);
	run_ms(2 * LINK_TX_MS + 150);
	memset(&got, 0, sizeof got);
	ap_sensors(1100);
	unsigned flushed = 0;
	for (unsigned i = 0; i < got.events; i++)
		flushed += got.ev[i].id == PROTO_SNS_META && got.ev[i].w[0] == PROTO_SNS_ACCEL;
	CHECK(flushed == 1);

	/* Without latency every sample goes out on its own; the step
	 * counter on activation and when it changes. */
	ap_batch(PROTO_SNS_ACCEL, 20000000, 0);
	steps = 100;
	ap_activate(PROTO_SNS_STEP_COUNTER, true, 0);
	memset(&got, 0, sizeof got);
	ap_sensors(1000);
	unsigned counted = 0;
	for (unsigned i = 0; i < got.events; i++)
		if (got.ev[i].id == PROTO_SNS_STEP_COUNTER) {
			counted++;
			CHECK(bits_float(got.ev[i].w[0]) == 100.0f);
		}
	CHECK(counted == 1 && got.events >= 1000 / 39 && got.reports >= got.events - 1);
	CHECK(companion_next_ms() > 1 && companion_next_ms() <= 39);

	/* A count the AP does not take is sent again. */
	steps = 120;
	run_ms(1000 + 2 * LINK_TX_MS + 150);
	memset(&got, 0, sizeof got);
	ap_sensors(1500);
	counted = 0;
	for (unsigned i = 0; i < got.events; i++)
		counted += got.ev[i].id == PROTO_SNS_STEP_COUNTER && bits_float(got.ev[i].w[0]) == 120.0f;
	CHECK(counted == 1);

	/* A warm reset with the AP keeping the panel keeps the
	 * subscriptions, and the times go on from the last one sent. */
	uint64_t last = got.last_time;
	clock_from_ms = now_ms;
	sns_start(true);
	memset(&got, 0, sizeof got);
	got.last_time = last;
	ap_sensors(200);
	CHECK(got.events >= 4 && !got.backwards);
	/* Any other start drops them. */
	sns_start(false);
	CHECK(!accel_shared);
	memset(&got, 0, sizeof got);
	ap_sensors(2000);
	CHECK(got.reports == 0);

	/* The heart rate sensor runs while the AP wants it; each new
	 * reading goes out at once, and one no-contact event after 15 s
	 * without one. */
	ap_activate(PROTO_SNS_HEART_RATE, true, 0);
	CHECK(hr_shared);
	memset(&got, 0, sizeof got);
	ap_sensors(2000);
	CHECK(got.reports == 0);
	hr_bpm = 64, hr_quality = 2, hr_readings = 1;
	ap_sensors(5);
	CHECK(got.events == 1 && got.ev[0].id == PROTO_SNS_HEART_RATE && got.ev[0].heart && got.ev[0].bpm == 64.0f &&
	      got.ev[0].status == PROTO_SNS_HIGH);
	CHECK(companion_next_ms() > 1000);
	hr_bpm = 66, hr_quality = 1, hr_readings = 2;
	CHECK(companion_next_ms() == 1);
	ap_sensors(5);
	CHECK(got.events == 2 && got.ev[1].bpm == 66.0f && got.ev[1].status == PROTO_SNS_MEDIUM);
	ap_sensors(14900);
	CHECK(got.events == 2);
	ap_sensors(200);
	CHECK(got.events == 3 && got.ev[2].heart && got.ev[2].bpm == 0 && got.ev[2].status == PROTO_SNS_NO_CONTACT);
	ap_sensors(20000);
	CHECK(got.events == 3);
	hr_readings = 3;
	ap_sensors(5);
	CHECK(got.events == 4 && got.ev[3].bpm == 66.0f);
	ap_activate(PROTO_SNS_HEART_RATE, false, 0);
	CHECK(!hr_shared);
	hr_readings = 4;
	ap_sensors(20000);
	CHECK(got.events == 4);
	ap_activate(PROTO_SNS_HEART_RATE, true, 0);
	CHECK(hr_shared);

	/* The gyroscope runs at the rate asked for, at most 52 Hz, in
	 * rad/s with low accuracy, and is off once the AP is done. */
	CHECK(lsm6dso_gyro_odr(0) == 0 && lsm6dso_gyro_odr(200) == 1 && lsm6dso_gyro_odr(80) == 1 &&
	      lsm6dso_gyro_odr(79) == 2 && lsm6dso_gyro_odr(39) == 2 && lsm6dso_gyro_odr(38) == 3 &&
	      lsm6dso_gyro_odr(20) == 3);
	ap_batch(PROTO_SNS_GYRO, 10000000, 0);
	CHECK(gyro_period == 0);
	ap_activate(PROTO_SNS_GYRO, true, 0);
	CHECK(gyro_period == 20);
	memset(&got, 0, sizeof got);
	ap_sensors(1000);
	unsigned turns = 0;
	uint64_t prev = 0;
	for (unsigned i = 0; i < got.events; i++)
		if (got.ev[i].id == PROTO_SNS_GYRO) {
			const float k = 70 * 0.001f * 3.14159265f / 180;
			CHECK(bits_float(got.ev[i].w[0]) == 1000 * k && bits_float(got.ev[i].w[1]) == -500 * k &&
			      bits_float(got.ev[i].w[2]) == 0 && got.ev[i].w[3] == PROTO_SNS_LOW);
			CHECK(!turns || got.ev[i].time - prev == 20000000);
			prev = got.ev[i].time;
			turns++;
		}
	CHECK(turns >= 1000 / 20 - 2 && turns <= 1000 / 20 + 1);
	CHECK(fabsf(bits_float(got.ev[0].w[0]) - 1.2217f) < 0.001f);
	ap_batch(PROTO_SNS_GYRO, 100000000, 0);
	CHECK(gyro_period == 100);
	ap_activate(PROTO_SNS_GYRO, false, 0);
	CHECK(gyro_period == 0);

	/* A gyroscope that does not take its rate is asked again a second
	 * later, then two seconds after that; going off likewise. */
	gyro_fails = 2;
	gyro_tries = 0;
	ap_activate(PROTO_SNS_GYRO, true, 0);
	CHECK(gyro_period == 0 && gyro_tries == 1 && companion_next_ms() <= 1000);
	ap_sensors(1100);
	CHECK(gyro_period == 0 && gyro_tries == 2);
	ap_sensors(1700);
	CHECK(gyro_period == 0 && gyro_tries == 2);
	ap_sensors(400);
	CHECK(gyro_period == 100 && gyro_tries == 3);
	ap_sensors(5000);
	CHECK(gyro_tries == 3);
	gyro_fails = 1;
	ap_activate(PROTO_SNS_GYRO, false, 0);
	CHECK(gyro_period == 100);
	ap_sensors(1100);
	CHECK(gyro_period == 0 && gyro_tries == 5);

	/* The magnetometer in µT on Android's axes (x the chip's -y, y
	 * its x) through the vendor's correction, unreliable as it is not
	 * calibrated. While it is on, the chip runs at 100 Hz and the
	 * gyroscope at 52 Hz, whatever rate the AP asks for. An overflow is
	 * left out. */
	CHECK(ak09918_mode(0) == 0 && ak09918_mode(1000) == 0x02 && ak09918_mode(100) == 0x02 &&
	      ak09918_mode(99) == 0x04 && ak09918_mode(50) == 0x04 && ak09918_mode(49) == 0x06 &&
	      ak09918_mode(21) == 0x06 && ak09918_mode(20) == 0x08 && ak09918_mode(10) == 0x08);
	mag_fails = 1;
	ap_activate(PROTO_SNS_MAG, true, 0);
	CHECK(mag_period == 0);
	ap_sensors(1100);
	CHECK(mag_period == 20 && gyro_period == 20);
	ap_batch(PROTO_SNS_MAG, 100000000, 0);
	CHECK(mag_period == 20);
	memset(&got, 0, sizeof got);
	ap_sensors(1000);
	float chip[3] = { -200 * 0.15f, 100 * 0.15f, -300 * 0.15f }, corrected[3];
	compass_correct(chip, corrected);
	unsigned fields = 0;
	for (unsigned i = 0; i < got.events; i++)
		if (got.ev[i].id == PROTO_SNS_MAG) {
			CHECK(fabsf(bits_float(got.ev[i].w[0]) - corrected[0]) < 0.01f &&
			      fabsf(bits_float(got.ev[i].w[1]) - corrected[1]) < 0.01f &&
			      fabsf(bits_float(got.ev[i].w[2]) - corrected[2]) < 0.01f &&
			      got.ev[i].w[3] == PROTO_SNS_UNRELIABLE);
			fields++;
		}
	CHECK(fields >= 9 && fields <= 11);
	mag_result = AK09918_OVERFLOW;
	memset(&got, 0, sizeof got);
	ap_sensors(1000);
	CHECK(got.events == 0);
	mag_result = AK09918_OK;

	/* Turned about each axis in turn with a hard-iron offset, the
	 * gyroscope turning with it, the field comes out without the
	 * offset and the accuracy goes up to high. The calibration is
	 * kept for the boot only, not stored. */
	static const float iron[3] = { 20, -35, 50 };
	float m[3] = { 0, 30, -36 };
	ap_batch(PROTO_SNS_MAG, 20000000, 0);
	memset(&got, 0, sizeof got);
	for (int k = 0; k < 600; k++) {
		float w[3] = { 0, 0, 0 }, wm[3];
		w[k / 50 % 3] = 2;
		wm[0] = w[1] * m[2] - w[2] * m[1], wm[1] = w[2] * m[0] - w[0] * m[2], wm[2] = w[0] * m[1] - w[1] * m[0];
		float n = 0;
		for (int i = 0; i < 3; i++) {
			m[i] -= wm[i] * 0.02f;
			n += m[i] * m[i];
		}
		for (int i = 0; i < 3; i++) {
			m[i] *= 46.86f / sqrtf(n);
			gyro_counts[i] = (int16_t)lroundf(w[i] / GYRO_RADS);
		}
		set_field(m, iron);
		ap_sensors(20);
	}
	gyro_counts[0] = gyro_counts[1] = gyro_counts[2] = 0;
	got.events = 0;
	ap_sensors(200);
	struct ap_event *calm = &got.ev[got.events - 1];
	CHECK(got.events >= 9 && calm->w[3] == PROTO_SNS_HIGH);
	CHECK(fabsf(bits_float(calm->w[0]) - m[0]) < 1 && fabsf(bits_float(calm->w[1]) - m[1]) < 1 &&
	      fabsf(bits_float(calm->w[2]) - m[2]) < 1);
	ap_activate(PROTO_SNS_MAG, false, 0);
	CHECK(mag_period == 0 && gyro_period == 0);
	run_ms(4000);
	CHECK(!kv_mag_len);
	ap_activate(PROTO_SNS_MAG, true, 0);
	memset(&got, 0, sizeof got);
	ap_sensors(100);
	CHECK(got.events >= 1 && got.ev[0].w[3] == PROTO_SNS_HIGH);
	ap_activate(PROTO_SNS_MAG, false, 0);

	/* The orientation runs the accelerometer, the gyroscope and the
	 * magnetometer while it is subscribed: face up with 12 o'clock to
	 * the north, then turned to the east in a second, with the field's
	 * accuracy. */
	static const int16_t flat[3] = { 0, 0, 1000 };
	static const float north[3] = { 0, 30, -36 };
	memcpy(accel_mg, flat, sizeof flat);
	set_field(north, iron);
	ap_batch(PROTO_SNS_ORIENTATION, 100000000, 0);
	ap_activate(PROTO_SNS_ORIENTATION, true, 0);
	CHECK(accel_shared && mag_period == 20 && gyro_period == 20);
	for (int k = 0; k < 2; k++) {
		for (int i = 1; k && i <= 50; i++) {
			float t = (float)i / 50 * 3.14159265f / 2, f[3] = { -30 * sinf(t), 30 * cosf(t), -36 };
			gyro_counts[2] = (int16_t)lroundf(-3.14159265f / 2 / GYRO_RADS);
			set_field(f, iron);
			ap_sensors(20);
		}
		gyro_counts[2] = 0;
		memset(&got, 0, sizeof got);
		ap_sensors(1000);
		CHECK(got.events >= 9 && got.events <= 11);
		struct ap_event *e = &got.ev[got.events - 1];
		float az = bits_float(e->w[0]);
		CHECK(e->id == PROTO_SNS_ORIENTATION && e->w[3] == PROTO_SNS_HIGH);
		CHECK(k ? fabsf(az - 90) < 2 : fabsf(az) < 2 || fabsf(az - 360) < 2);
		CHECK(fabsf(bits_float(e->w[1])) < 0.5f && fabsf(bits_float(e->w[2])) < 0.5f);
	}
	ap_activate(PROTO_SNS_ORIENTATION, false, 0);
	CHECK(!accel_shared && mag_period == 0 && gyro_period == 0);

	/* The watch's own compass runs the same chips with the same
	 * calibration, sends the AP nothing and keeps going through the
	 * AP's orientation coming and going and the AP going down: turned
	 * from the north to the east, the heading follows. */
	float heading = -1;
	CHECK(sns_heading(&heading) == -1);
	set_field(north, iron);
	sns_compass(true);
	CHECK(accel_shared && mag_period == 20 && gyro_period == 20);
	memset(&got, 0, sizeof got);
	ap_sensors(500);
	CHECK(got.events == 0);
	CHECK(sns_heading(&heading) == 3 && (heading < 2 || heading > 358));
	ap_activate(PROTO_SNS_ORIENTATION, true, 0);
	ap_activate(PROTO_SNS_ORIENTATION, false, 0);
	sns_reset();
	CHECK(accel_shared && mag_period == 20 && gyro_period == 20);
	for (int i = 1; i <= 50; i++) {
		float t = (float)i / 50 * 3.14159265f / 2, f[3] = { -30 * sinf(t), 30 * cosf(t), -36 };
		gyro_counts[2] = (int16_t)lroundf(-3.14159265f / 2 / GYRO_RADS);
		set_field(f, iron);
		ap_sensors(20);
	}
	gyro_counts[2] = 0;
	ap_sensors(500);
	CHECK(sns_heading(&heading) == 3 && fabsf(heading - 90) < 2);

	/* The panel handed to the AP stops the chips the watch asked for,
	 * those of the AP's orientation going on, and they start again
	 * when the watch has it back. */
	static const uint8_t m375[] = { 0x08, 0x01 };
	ap_write(1, 375, m375, sizeof m375);
	companion_pass();
	run_ms(3000);
	CHECK(!power_lent && mag_period == 20 && gyro_period == 20);
	ap_activate(PROTO_SNS_ORIENTATION, true, 0);
	mode_message(373, NULL, 0, &a);
	CHECK(power_lent && accel_shared && mag_period == 20 && gyro_period == 20 && sns_heading(&heading) == -1);
	ap_activate(PROTO_SNS_ORIENTATION, false, 0);
	CHECK(!accel_shared && mag_period == 0 && gyro_period == 0);
	ap_write(1, 375, m375, sizeof m375);
	companion_pass();
	run_ms(3000);
	CHECK(!power_lent && accel_shared && mag_period == 20 && gyro_period == 20);
	ap_sensors(500);
	CHECK(sns_heading(&heading) == 3);
	/* A 374 stops them before a sample reads the bus, and a stop the
	 * magnetometer does not take is asked again. */
	mag_fails = 1;
	mode_message(374, NULL, 0, &a);
	CHECK(a.mode == PROTO_MODE_POWER_OFF && power_lent && hub_down);
	CHECK(!accel_shared && gyro_period == 0 && mag_period == 20 && sns_heading(&heading) == -1);
	ap_sensors(1100);
	CHECK(mag_period == 0);
	ap_write(1, 375, m375, sizeof m375);
	companion_pass();
	run_ms(3000);
	CHECK(!power_lent && !hub_down && mag_period == 20 && gyro_period == 20);
	ap_sensors(500);
	CHECK(sns_heading(&heading) >= 0);
	/* Without a field for a while the heading is not given. */
	mag_result = AK09918_NONE;
	ap_sensors(400);
	CHECK(sns_heading(&heading) >= 0);
	ap_sensors(200);
	CHECK(sns_heading(&heading) == -1);
	mag_result = AK09918_OK;
	ap_sensors(100);
	CHECK(sns_heading(&heading) >= 0);
	/* Nor at any accuracy while the field is out of range. */
	mag_result = AK09918_OVERFLOW;
	ap_sensors(40);
	CHECK(sns_heading(&heading) == 0);
	ap_sensors(10000);
	CHECK(sns_heading(&heading) <= 0);
	mag_result = AK09918_OK;
	sns_compass(false);
	CHECK(!accel_shared && mag_period == 0 && gyro_period == 0 && sns_heading(&heading) == -1);

	/* A sample the watch's compass left is not sent to an AP that
	 * subscribes to the field later. */
	sns_compass(true);
	ap_sensors(100);
	sns_compass(false);
	mode_message(373, NULL, 0, &a);
	run_ms(5000);
	mag_result = AK09918_NONE;
	memset(&got, 0, sizeof got);
	ap_activate(PROTO_SNS_MAG, true, 0);
	ap_sensors(300);
	CHECK(got.events == 0);
	mag_result = AK09918_OK;
	ap_sensors(300);
	CHECK(got.events >= 1);
	ap_activate(PROTO_SNS_MAG, false, 0);
	CHECK(mag_period == 0);
	static const int16_t tilted[3] = { 0, -1000, 500 }, counts[3] = { 100, 200, -300 };
	memcpy(accel_mg, tilted, sizeof tilted);
	memcpy(mag_counts, counts, sizeof counts);

	/* The light sensor is powered while the AP wants it, and sends
	 * lux when it changes, read at most once an integration. */
	ap_batch(PROTO_SNS_LIGHT, 20000000, 0);
	ap_activate(PROTO_SNS_LIGHT, true, 0);
	CHECK(light_shared);
	memset(&got, 0, sizeof got);
	ap_sensors(500);
	CHECK(got.events == 0);
	lux = 0;
	ap_sensors(150);
	CHECK(got.events == 1 && got.ev[0].id == PROTO_SNS_LIGHT && bits_float(got.ev[0].w[0]) == 0);
	ap_sensors(1000);
	CHECK(got.events == 1);
	lux = 328;
	CHECK(companion_next_ms() <= 100);
	ap_sensors(150);
	CHECK(got.events == 2 && bits_float(got.ev[1].w[0]) == 328.0f);
	ap_activate(PROTO_SNS_LIGHT, false, 0);
	CHECK(!light_shared);
	ap_activate(PROTO_SNS_LIGHT, true, 0);
	ap_sensors(150);
	CHECK(got.events == 3 && bits_float(got.ev[2].w[0]) == 328.0f);

	/* A change under 2 lux and a twentieth is jitter. */
	lux = 344;
	ap_sensors(500);
	CHECK(got.events == 3);
	lux = 345;
	ap_sensors(150);
	CHECK(got.events == 4 && bits_float(got.ev[3].w[0]) == 345.0f);
	lux = 0;
	ap_sensors(150);
	lux = 1;
	ap_sensors(150);
	CHECK(got.events == 5 && bits_float(got.ev[4].w[0]) == 0);

	/* A lux the AP does not take is sent again. */
	lux = 400;
	for (int i = 0; i < 150 && !levels[UP_INT]; i++)
		wait_ms(1);
	CHECK(levels[UP_INT]);
	run_ms(2 * LINK_TX_MS + 150);
	ap_sensors(300);
	CHECK(got.events >= 6 && bits_float(got.ev[got.events - 1].w[0]) == 400.0f);

	/* The light sensor not powering up is asked again too. */
	light_fails = true;
	light_tries = 0;
	ap_sensors(1100);
	CHECK(light_tries == 1);
	light_fails = false;
	ap_sensors(5000);
	CHECK(light_tries == 1);

	/* Power off drops them. */
	ap_batch(PROTO_SNS_ACCEL, 20000000, 1000000000);
	ap_activate(PROTO_SNS_ACCEL, true, 0);
	CHECK(accel_shared);
	mode_message(374, NULL, 0, &a);
	CHECK(a.mode == PROTO_MODE_POWER_OFF && !accel_shared && !hr_shared && !light_shared);
	memset(&got, 0, sizeof got);
	ap_sensors(2000);
	CHECK(got.reports == 0);
	mode_message(373, NULL, 0, &a);

	/* The daemon's 386, also after a restart of the daemon alone,
	 * keeps them; a malformed one changes nothing. */
	ap_activate(PROTO_SNS_ACCEL, true, 0);
	CHECK(accel_shared);
	ap_sensors(1);
	ap_write(1, PROTO_BOOT, NULL, 0);
	companion_pass();
	static const uint8_t cut386[] = { 0x1a, 0x02, 0x08, 0x80 };
	uint32_t rtc = rtc_writes;
	ap_write(1, PROTO_BOOT, cut386, sizeof cut386);
	companion_pass();
	CHECK(accel_shared && rtc_writes == rtc);
	ap_activate(PROTO_SNS_ACCEL, false, 0);

	/* A no-contact event the AP does not take is sent again. */
	ap_activate(PROTO_SNS_HEART_RATE, true, 0);
	run_ms(15000);
	CHECK(levels[UP_INT]);
	run_ms(2 * LINK_TX_MS + 150);
	memset(&got, 0, sizeof got);
	ap_sensors(300);
	CHECK(got.events == 1 && got.ev[0].heart && got.ev[0].status == PROTO_SNS_NO_CONTACT);
	ap_sensors(20000);
	CHECK(got.events == 1);
	ap_activate(PROTO_SNS_HEART_RATE, false, 0);

	/* Wrist tilt turns the hub's detection on while it is subscribed.
	 * A raise goes out at once; one the AP does not take, or that
	 * finds the queue full, goes again. */
	CHECK(!tilt_shared);
	ap_activate(PROTO_SNS_WRIST_TILT, true, 0);
	CHECK(tilt_shared);
	memset(&got, 0, sizeof got);
	tilts++;
	ap_sensors(5);
	CHECK(got.events == 1 && got.ev[0].id == PROTO_SNS_WRIST_TILT && bits_float(got.ev[0].w[0]) == 1.0f);
	tilts++;
	for (int i = 0; i < 5 && !levels[UP_INT]; i++)
		wait_ms(1);
	CHECK(levels[UP_INT]);
	run_ms(2 * LINK_TX_MS + 150);
	ap_sensors(300);
	CHECK(got.events == 2 && got.ev[1].id == PROTO_SNS_WRIST_TILT);
	ap_batch(PROTO_SNS_ACCEL, 20000000, 0);
	ap_activate(PROTO_SNS_ACCEL, true, 0);
	run_ms(PROTO_SNS_EVENTS * 39 + 50);
	tilts++;
	run_ms(2 * LINK_TX_MS + 150);
	memset(&got, 0, sizeof got);
	ap_sensors(500);
	unsigned raised = 0;
	for (unsigned i = 0; i < got.events; i++)
		raised += got.ev[i].id == PROTO_SNS_WRIST_TILT;
	CHECK(raised == 1);
	ap_activate(PROTO_SNS_ACCEL, false, 0);
	ap_activate(PROTO_SNS_WRIST_TILT, false, 0);
	CHECK(!tilt_shared);
	memset(&got, 0, sizeof got);
	tilts++;
	ap_sensors(100);
	CHECK(got.events == 0);

	/* No reports in the wristband mode, where the AP may be off and
	 * its boot loader needs up_int low; they go again in the normal
	 * mode. */
	ap_activate(PROTO_SNS_WRIST_TILT, true, 0);
	ap_write(1, 375, m375, sizeof m375);
	companion_pass();
	tilts++;
	for (int i = 0; i < 3000; i++) {
		wait_ms(1);
		CHECK(!levels[UP_INT]);
	}
	mode_message(373, NULL, 0, &a);
	memset(&got, 0, sizeof got);
	ap_sensors(100);
	CHECK(got.events == 1 && got.ev[0].id == PROTO_SNS_WRIST_TILT);
	ap_activate(PROTO_SNS_WRIST_TILT, false, 0);
}

static char status[4096];
static unsigned status_n;

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(status + status_n, sizeof status - status_n, fmt, ap);
	va_end(ap);
	if (n > 0 && status_n + (unsigned)n < sizeof status)
		status_n += (unsigned)n;
}

static bool status_has(const char *s)
{
	status_n = 0;
	status[0] = 0;
	companion_status(say);
	return strstr(status, s) != NULL;
}

/* Who has the panel after each kind of reset, from each stored mode.
 * A power-on before each one leaves the kept mode the stored one. */
static void boot_decisions(void)
{
	enum { REL = 14 };
	static const struct { uint32_t bits; bool pin, powered; } resets[] = {
		{ 1u << 0, true, false },	/* the AP's pin */
		{ 1u << 0 | 1u << 1, true, true },
		{ 1u << 1, false, true },	/* power-on */
		{ 1u << 2, false, true },	/* brown-out */
		{ 1u << 3, false, false },	/* software POR */
		{ 1u << 4, false, false },	/* software POI */
		{ 1u << 5, false, false },	/* debugger, as swdtool flash */
		{ 1u << 6, false, false },	/* watchdog */
	};
	static const char *const names[] = { "normal", "wristband", "power off", "power off" };
	char want[64];
	for (unsigned r = 0; r < sizeof resets / sizeof resets[0]; r++)
		for (int m = PROTO_MODE_NORMAL; m <= STORED_OFF; m++) {
			kv_mode = m;
			reset_bits = 1u << 1;
			companion_boot();
			reset_bits = resets[r].bits;
			levels[OWN] = 1;
			outputs[OWN] = true;
			levels[REL] = 0;
			outputs[REL] = true;
			bool lent = companion_boot(), want_lent = resets[r].pin || m == STORED_OFF ||
								    (m == PROTO_MODE_NORMAL && !resets[r].powered);
			CHECK(lent == want_lent);
			if (resets[r].pin)
				snprintf(want, sizeof want, "normal mode, %s before", names[m]);
			else
				snprintf(want, sizeof want, "%s mode, %s before", names[m], names[m]);
			if (!status_has(want))
				printf("reset %08x, stored %s: %s", resets[r].bits, names[m], status);
			CHECK(status_has(want));
			CHECK(status_has("nobody has the panel") == (m == STORED_OFF && !resets[r].pin));
			if (resets[r].pin)
				CHECK(!levels[OWN] && outputs[OWN] && levels[REL] && outputs[REL]);
			else if (lent)
				CHECK(!levels[OWN] && outputs[OWN] && !outputs[REL]);
			else
				CHECK(levels[OWN] && outputs[REL]);
		}

	/* The normal mode is stored after the AP's reset without a 373,
	 * and the 377 says where it came from. */
	struct ap_reply a;
	kv_mode = PROTO_MODE_WRISTBAND;
	reset_bits = 1u << 0;
	CHECK(companion_boot());
	run_ms(STORE_WAIT_MS);
	CHECK(kv_mode == PROTO_MODE_NORMAL);
	mode_message(PROTO_MODE_REQUEST, NULL, 0, &a);
	CHECK(a.mode == PROTO_MODE_NORMAL && a.last_mode == PROTO_MODE_WRISTBAND && a.reboot == PROTO_REBOOT_PIN);
}

/* Timepiece mode from the AP, then the watch's own reboot before the
 * mode is in the store: it starts with the panel again. */
static void reboot_in_wristband(void)
{
	static const uint8_t m375[] = { 0x08, 0x01 };
	int stored = kv_mode;
	ap_write(1, 375, m375, sizeof m375);
	companion_pass();
	run_ms(1000);
	CHECK(!display_lent && levels[OWN] && kv_mode == stored);
	reset_bits = 1u << 4;
	CHECK(!companion_boot());
	companion_init();
	wait_ms(1);
	CHECK(!display_lent && !touch_lent && levels[OWN] && !levels[UP_INT] && !levels[DN_ACK] && irq_on[DN_INT]);
	CHECK(status_has("wristband mode") && status_has("the watch has the panel"));
}

static void pin_reset_lends(void)
{
	reset_bits = 1u << 0;
	CHECK(companion_boot());
	companion_init();
	CHECK(display_lent && touch_lent && !levels[OWN] && status_has("normal mode, "));
	CHECK(status_has("the AP has the panel"));
	run_ms(STORE_WAIT_MS);
	CHECK(kv_mode == PROTO_MODE_NORMAL);
}

/* The AP boots after the watch rebooted itself in wristband mode. */
static void reboot_scenario(void)
{
	/* From the power key the notice is low: no answer, the lines stay
	 * low for the boot loader's check, and its reset lends. */
	reboot_in_wristband();
	levels[DN_INT] = 1;
	fire(DN_INT);
	wait_ms(75);
	CHECK(!levels[DN_ACK] && !levels[UP_INT] && !display_lent);
	levels[DN_INT] = 0;
	pin_reset_lends();

	/* From the charger it is answered, and its reset lends. */
	reboot_in_wristband();
	lk_answered();
	wait_ms(200);
	pin_reset_lends();

	/* A reset the watch does itself puts the mode in the store first,
	 * so it holds without anything kept in RAM: a power-on reads the
	 * store alone. */
	pin_reset_lends();
	static const uint8_t m375[] = { 0x08, 0x01 };
	ap_write(1, 375, m375, sizeof m375);
	companion_pass();
	run_ms(1000);
	unsigned sets = kv_sets;
	CHECK(kv_mode == PROTO_MODE_NORMAL && !display_lent);
	companion_flush();
	CHECK(kv_mode == PROTO_MODE_WRISTBAND && kv_sets == sets + 1);
	companion_flush();
	CHECK(kv_sets == sets + 1);
	reset_bits = 1u << 1;
	CHECK(!companion_boot());
	companion_init();
	CHECK(status_has("wristband mode") && status_has("the watch has the panel"));

	/* The mode from before the reboot is stored after it. */
	reboot_in_wristband();
	run_ms(STORE_WAIT_MS);
	CHECK(kv_mode == PROTO_MODE_WRISTBAND && !display_lent);

	/* Without a reset the AP still gets the panel, and the event
	 * history says so. */
	noted[0] = 0;
	lk_answered();
	CHECK(!noted[0]);
	wait_ms(1000);
	CHECK(status_has("normal mode, wristband before") && strstr(noted, "no reset after the boot loader"));
	CHECK(display_lent && touch_lent && !levels[OWN]);
}

int main(void)
{
	sensor_tests();

	/* The battery preference of the AsteroidOS backend, capacity 100,
	 * on channel 1 behind id 372. */
	static const uint8_t body372[] = { 0x08, 0x11, 0x7a, 0x02, 0x10, 0x64 };
	CHECK(wire_sum(body372, sizeof body372) == 0x886d);
	uint8_t env[32];
	CHECK(wire_wrap(env, sizeof env, 1, 372, body372, sizeof body372) == 14);
	static const uint8_t env372[] = { 1, 0, 10, 0, 0x74, 0x01, 6, 0, 0x08, 0x11, 0x7a, 0x02, 0x10, 0x64 };
	CHECK(!memcmp(env, env372, sizeof env372));
	uint8_t h[8];
	wire_header_put(h, 5, 1, env372, sizeof env372);
	static const uint8_t h372[] = { 0x05, 0x00, 0x01, 0x7d, 0xe8, 0x86, 0x0e, 0x00 };
	CHECK(!memcmp(h, h372, 8));
	unsigned sum = 0;
	for (unsigned i = 0; i < 8; i++)
		sum += h[i];
	CHECK((sum & 0xff) == 0xff);
	struct wire_header hd;
	CHECK(wire_header_get(h, &hd) == WIRE_OK && hd.seq == 5 && hd.flags == 1 && hd.sum == 0x86e8 && hd.len == 14);
	h[1] ^= 0x10;
	CHECK(wire_header_get(h, &hd) == WIRE_BAD_HEADER);
	uint8_t big[8];
	wire_header_put(big, 0, 0, env372, 0);
	big[6] = 0x09, big[7] = 0x02, big[3] = (uint8_t)(big[3] - 0x09 - 0x02);
	CHECK(wire_header_get(big, &hd) == WIRE_TOO_LONG);

	/* An odd byte is the low byte of a word; the carry folds in once. */
	static const uint8_t ff[5] = { 0xff, 0xff, 0xff, 0xff, 0xff };
	CHECK(wire_sum(ff, 5) == 0xff00);
	CHECK(wire_sum(ff, 0) == 0xffff);

	struct wire_msg m;
	CHECK(wire_unwrap(env372, sizeof env372, &m) && m.framework && m.chan == 1 && m.id == 372 && m.len == 6 &&
	      m.body == env372 + 8);
	CHECK(!wire_unwrap(env372, 13, &m));
	static const uint8_t sns[] = { 4, 0, 3, 0, 0xaa, 0xbb, 0xcc };
	CHECK(wire_unwrap(sns, sizeof sns, &m) && !m.framework && m.chan == 4 && m.len == 3 && m.body == sns + 4);
	static const uint8_t short_body[] = { 0, 0, 4, 0, 0x74, 0x01, 1, 0 };
	CHECK(!wire_unwrap(short_body, sizeof short_body, &m));

	/* Parts each way, across the boundaries of the first part and of
	 * the full ones. */
	static const unsigned lens[] = { 0, 1, 14, 111, 112, 113, 232, 233, 352, 353, 512, 520 };
	for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++)
		roundtrip_write(lens[i]);
	CHECK(wire_write_parts(112) == 1 && wire_write_parts(113) == 2 && wire_write_parts(232) == 2 &&
	      wire_write_parts(233) == 3 && wire_write_parts(520) == 5);
	static const unsigned rlens[] = { 0, 14, 112, 113, 240, 241, 488, 489, 520 };
	for (unsigned i = 0; i < sizeof rlens / sizeof rlens[0]; i++)
		roundtrip_read(rlens[i]);
	CHECK(wire_read_parts(240) == 1 && wire_read_parts(241) == 2 && wire_read_parts(520) == 3);

	/* A part with a bad payload or header. */
	uint8_t parts[5][121];
	uint8_t payload[200];
	memset(payload, 0x5a, sizeof payload);
	ap_write_parts(payload, sizeof payload, 1, parts);
	parts[1][3] ^= 1;
	struct wire_rx rx;
	enum wire_error err;
	wire_rx_start(&rx);
	CHECK(wire_rx_part(&rx, parts[0], &err) == WIRE_RX_MORE);
	CHECK(wire_rx_part(&rx, parts[1], &err) == WIRE_RX_BAD && err == WIRE_BAD_SUM);
	parts[0][0] ^= 1;
	wire_rx_start(&rx);
	CHECK(wire_rx_part(&rx, parts[0], &err) == WIRE_RX_BAD && err == WIRE_BAD_HEADER);

	/* Sequence numbers: any after a start, then the next one; one out of
	 * turn is refused and expected next, unless it asks to resync. */
	struct wire_seq s = { 0, true };
	struct wire_header q = { .seq = 40 };
	CHECK(wire_seq_take(&s, &q) && s.expect == 41 && !s.resync);
	q.seq = 41;
	CHECK(wire_seq_take(&s, &q) && s.expect == 42);
	q.seq = 41;
	CHECK(!wire_seq_take(&s, &q) && s.expect == 41);
	CHECK(wire_seq_take(&s, &q) && s.expect == 42);
	q.seq = 7, q.flags = WIRE_FLAG_RESYNC;
	CHECK(wire_seq_take(&s, &q) && s.expect == 8);
	q.seq = 0xffff, q.flags = 0;
	s.expect = 0xffff;
	CHECK(wire_seq_take(&s, &q) && s.expect == 0);

	/* The zone is a sint32 in minutes. */
	CHECK(proto_zigzag(0) == 0 && proto_zigzag(-1) == 1 && proto_zigzag(1) == 2 && proto_zigzag(120) == 240);
	CHECK(proto_zigzag(-300) == 599 && proto_zigzag(INT32_MIN) == 0xffffffffu);
	static const int32_t zones[] = { 0, 60, -60, 330, -570, 840, INT32_MAX, INT32_MIN };
	for (unsigned i = 0; i < sizeof zones / sizeof zones[0]; i++)
		CHECK(proto_unzigzag(proto_zigzag(zones[i])) == zones[i]);

	/* 373 as the backend writes it: mode 0 left out, the time at UTC+2. */
	static const uint8_t m373[] = { 0x12, 0x09, 0x08, 0x80, 0xe2, 0xcf, 0xaa, 0x06, 0x18, 0xf0, 0x01 };
	unsigned mode = 9;
	struct proto_time t;
	CHECK(proto_work_mode(m373, sizeof m373, &mode, &t) && mode == PROTO_MODE_NORMAL);
	CHECK(t.time == 1700000000 && t.zone_min == 120);
	CHECK(proto_mode_of(373) == PROTO_MODE_NORMAL && proto_mode_of(375) == PROTO_MODE_WRISTBAND &&
	      proto_mode_of(374) == PROTO_MODE_POWER_OFF && proto_mode_of(372) == -1);
	/* 375 at UTC-5. */
	static const uint8_t m375[] = { 0x08, 0x01, 0x12, 0x09, 0x08, 0x80, 0xe2, 0xcf, 0xaa, 0x06, 0x18, 0xd7, 0x04 };
	CHECK(proto_work_mode(m375, sizeof m375, &mode, &t) && mode == PROTO_MODE_WRISTBAND);
	CHECK(t.time == 1700000000 && t.zone_min == -300);
	CHECK(!proto_work_mode(m375, sizeof m375 - 1, &mode, &t));
	/* A time whose own fields are cut short, inside a whole message. */
	static const uint8_t cut[] = { 0x08, 0x01, 0x12, 0x02, 0x08, 0x80 };
	CHECK(!proto_work_mode(cut, sizeof cut, &mode, &t));
	static const uint8_t cut386[] = { 0x1a, 0x02, 0x08, 0x80 };
	CHECK(!proto_boot(cut386, sizeof cut386, &t));
	static const uint8_t m374[] = { 0x08, 0x02, 0x12, 0x00 };
	CHECK(proto_work_mode(m374, sizeof m374, &mode, &t) && mode == PROTO_MODE_POWER_OFF && !t.time && !t.zone_min);

	/* 372: capacity 100 behind cmd 17; another cmd is not a battery. */
	CHECK(proto_battery(body372, sizeof body372) == 100);
	static const uint8_t b0[] = { 0x08, 0x11, 0x7a, 0x00 };
	CHECK(proto_battery(b0, sizeof b0) == 0);
	static const uint8_t other[] = { 0x08, 0x10, 0x7a, 0x02, 0x10, 0x64 };
	CHECK(proto_battery(other, sizeof other) == -1);
	CHECK(proto_battery(body372, 1) == -1);
	static const uint8_t cut372[] = { 0x08, 0x11, 0x7a, 0x02, 0x10, 0x80 };
	CHECK(proto_battery(cut372, sizeof cut372) == -1);

	/* 386: type, OOBE, then the time. */
	static const uint8_t m386[] = { 0x08, 0x02, 0x12, 0x02, 0x08, 0x01, 0x1a, 0x09,
					0x08, 0x80, 0xe2, 0xcf, 0xaa, 0x06, 0x18, 0xf0, 0x01 };
	CHECK(proto_boot(m386, sizeof m386, &t) && t.time == 1700000000 && t.zone_min == 120);

	/* 377 as the backend reads it. */
	struct proto_reply r = { PROTO_MODE_WRISTBAND, PROTO_MODE_NORMAL, PROTO_REBOOT_FAULT, { 1700000000, -300 } };
	uint8_t out[64];
	int n = proto_reply_put(out, sizeof out, &r);
	static const uint8_t m377[] = { 0x08, 0x01, 0x22, 0x09, 0x10, 0x80, 0xe2, 0xcf, 0xaa, 0x06, 0x18, 0xd7, 0x04, 0x28, 0x02 };
	CHECK(n == sizeof m377 && !memcmp(out, m377, sizeof m377));
	struct ap_reply a;
	ap_parse(out, n, &a);
	CHECK(a.mode == 1 && a.last_mode == 0 && a.reboot == 2 && a.time == 1700000000 && a.zone == -300);
	r = (struct proto_reply){ PROTO_MODE_NORMAL, PROTO_MODE_POWER_OFF, PROTO_REBOOT_DEBUG, { 0, 0 } };
	n = proto_reply_put(out, sizeof out, &r);
	CHECK(n == 2 && out[0] == 0x18 && out[1] == 0x02);
	ap_parse(out, n, &a);
	CHECK(a.mode == 0 && a.last_mode == 2 && a.reboot == 0 && a.time == 0);
	CHECK(proto_reply_put(out, 1, &r) == -1);
	/* Every field at 0 is an empty body, not a failure. */
	r = (struct proto_reply){ PROTO_MODE_NORMAL, PROTO_MODE_NORMAL, PROTO_REBOOT_DEBUG, { 0, 0 } };
	CHECK(proto_reply_put(out, sizeof out, &r) == 0);

	/* The reboot reason from RSTGEN's status. */
	CHECK(proto_reboot_reason(1u << 5 | 1u << 6, false) == PROTO_REBOOT_FAULT);
	CHECK(proto_reboot_reason(1u << 5 | 1u << 0, false) == PROTO_REBOOT_PIN);
	CHECK(proto_reboot_reason(1u << 5, false) == PROTO_REBOOT_DEBUG);
	CHECK(proto_reboot_reason(1u << 6, false) == PROTO_REBOOT_FAULT);
	CHECK(proto_reboot_reason(1u << 4, true) == PROTO_REBOOT_FAULT);
	CHECK(proto_reboot_reason(1u << 0, false) == PROTO_REBOOT_PIN);
	CHECK(proto_reboot_reason(1u << 0 | 1u << 1, false) == PROTO_REBOOT_PIN);
	CHECK(proto_reboot_reason(1u << 4, false) == PROTO_REBOOT_OTHER);
	CHECK(proto_powered_up(1u << 1) && proto_powered_up(1u << 2 | 1u << 0));
	CHECK(!proto_powered_up(1u << 0) && !proto_powered_up(1u << 3) && !proto_powered_up(1u << 5 | 1u << 6));

	companion_scenario();
	sensor_scenario();
	boot_decisions();
	reboot_scenario();

	if (failures)
		printf("companion_test: %d failures\n", failures);
	return failures != 0;
}
