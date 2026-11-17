/* The framing of beluga's AP link: the AP's side of it is played here
 * as deamonserver does it, its writes and its reads. Then the link and
 * the work modes run against a played AP, with the hardware and the
 * rest of the firmware stubbed out. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "companion/wire.h"
#include "companion/proto.h"
#include "companion/link.h"
#include "companion/companion.h"
#include "companion/sensors.h"
#include "board/board.h"
#include "sensors/hub.h"
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
enum { UP_INT = 29, DN_INT = 30, DN_ACK = 31, UP_ACK = 33, PINS = 64 };
#define OWN 36
static const struct board test_board = { .ap_link = { UP_INT, UP_ACK, DN_INT, DN_ACK, OWN, 14 } };
const struct board *board = &test_board;

static uint32_t now_ms, extra_us, reset_bits, rtc_writes, rtc_last, kv_sets, wakes, darks, chips_offs;
static unsigned levels[PINS];
static bool irq_on[PINS], display_lent, touch_lent, haptics_held, power_lent, key_down;
static void (*irq_fn[PINS])(unsigned);
static void (*ios_fn)(uint32_t);
static uint8_t lram[120], staged[256];
static unsigned staged_n;
static int kv_mode = -1;
static int32_t kv_zone;
static uint32_t steps = HUB_NONE, tilts, clock_from_ms, ms_per_read;
static bool accel_shared;

uint32_t time_ms(void) { return now_ms += ms_per_read; }
uint32_t time_us(void) { return now_ms * 1000 + extra_us; }
uint32_t irq_save(void) { return 0; }
void irq_restore(uint32_t s) {}
bool sched_add(struct sched_task *t) { return true; }
void sched_post(const struct sched_task *t) {}
void gpio_config(unsigned n, unsigned func, unsigned dir, unsigned pull, unsigned drive_ma) {}
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
int kv_get(const char *key, void *buf, unsigned size)
{
	if (!strcmp(key, "ap.mode") && kv_mode >= 0) {
		*(uint8_t *)buf = (uint8_t)kv_mode;
		return 1;
	}
	return -2;
}
int kv_set(const char *key, const void *val, unsigned len)
{
	kv_sets++;
	if (!strcmp(key, "ap.mode"))
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
bool rtc_valid(void) { return rtc_writes != 0; }
void rtc_write(uint32_t secs)
{
	rtc_writes++;
	rtc_last = secs;
}
void battery_from_ap(unsigned percent) {}
void hub_set_time(void) {}
void display_lend(bool to_ap) { display_lent = to_ap; }
void display_start_lent(void) { display_lent = true; }
void touch_lend(bool to_ap) { touch_lent = to_ap; }
void touch_start_lent(void) { touch_lent = true; }
void haptics_hold(bool on) { haptics_held = on; }
void power_lend_display(bool on) { power_lent = on; }
void power_dark_until_wake(void) { darks++; }
bool board_chips_off(void)
{
	chips_offs++;
	return true;
}
void iom_stop(unsigned bus) {}
void power_wake(const char *cause) { wakes++; }
bool power_key(void) { return key_down; }
void hub_accel_share(bool on) { accel_shared = on; }
bool hub_accel(int16_t v[3])
{
	v[0] = 0, v[1] = -1000, v[2] = 500;
	return accel_shared;
}
uint32_t hub_steps(void) { return steps; }
uint32_t hub_tilts(void) { return tilts; }
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
 * whose field 1 is SensorsVecT, four fixed32). Every field is checked
 * for its wire type; -1 for anything else. */
struct ap_event { uint32_t id; uint64_t time; uint32_t w[4]; unsigned words; };

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
		if (k3 != (3 << 3 | 2) || j + l3 != m)
			return -1;
		const uint8_t *r = q + j;
		unsigned rn = (unsigned)l3, t = 0;
		uint64_t k1 = ap_varint(r, rn, &t), l1 = ap_varint(r, rn, &t);
		if (k1 != (1 << 3 | 2) || t + l1 != rn || ap_vec(r + t, (unsigned)l1, e))
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
		CHECK(levels[UP_INT] || (companion_next_ms() > 0 && companion_next_ms() <= LINK_DN_SETTLE_MS));
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
	CHECK(display_lent && touch_lent && haptics_held && !levels[OWN] && !chips_offs);
	companion_init();
	CHECK(power_lent && irq_on[DN_INT]);

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
	 * one; the panel comes back a second later, the store waits for the
	 * end of the boot burst and a quiet link. */
	ms_per_read = 1;
	ap_write(1, 375, m375, sizeof m375);
	companion_pass();
	ms_per_read = 0;
	CHECK(!levels[UP_INT] && rtc_writes == 1);
	CHECK(display_lent && power_lent);
	uint32_t left = companion_next_ms();
	CHECK(left > 900 && left <= 1000);
	wait_ms(left - 1);
	CHECK(display_lent);
	wait_ms(1);
	CHECK(!display_lent && !touch_lent && !haptics_held && !power_lent && wakes == 1 && !kv_sets && levels[OWN]);
	/* The chips on the AP's bus are put to sleep once it is the watch's. */
	CHECK(chips_offs == 1);
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
	wait_ms(11000);
	wait_ms(1);
	CHECK(!display_lent && !power_lent && kv_sets == 2);

	/* 373, then 374: taken back without waking. */
	mode_message(373, NULL, 0, &a);
	CHECK(a.mode == PROTO_MODE_NORMAL && a.last_mode == PROTO_MODE_WRISTBAND);
	CHECK(display_lent && touch_lent && haptics_held && power_lent && !levels[OWN]);
	/* A reset before the store has the normal mode keeps the AP's
	 * panel. */
	CHECK(kv_mode == PROTO_MODE_WRISTBAND);
	reset_bits = 1u << 0;
	CHECK(companion_boot() && display_lent && !levels[OWN]);
	mode_message(374, m374, sizeof m374, &a);
	CHECK(a.mode == PROTO_MODE_POWER_OFF && a.last_mode == PROTO_MODE_NORMAL && display_lent);
	wait_ms(1000);
	CHECK(!display_lent && !power_lent && wakes == 1 && darks == 1 && chips_offs == 2);

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
	CHECK(!display_lent && wakes == 2 && kv_sets == sets + 1 && kv_mode == PROTO_MODE_POWER_OFF);
	key_down = false;

	/* A 374 whose 377 the AP never reads, as it is on its way down:
	 * power off all the same, and the panel back a second later. */
	mode_message(373, NULL, 0, &a);
	CHECK(display_lent && !levels[OWN]);
	ap_write(1, 374, m374, sizeof m374);
	companion_pass();
	CHECK(levels[UP_INT] && display_lent);
	wait_ms(999);
	CHECK(display_lent);
	wait_ms(1);
	CHECK(!display_lent && levels[OWN] && wakes == 2 && darks == 2);
	run_ms(2 * LINK_TX_MS + 150);
	CHECK(!levels[UP_INT]);
	run_ms(STORE_WAIT_MS);
	CHECK(kv_mode == PROTO_MODE_POWER_OFF);

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
	CHECK(!display_lent && !power_lent && levels[OWN]);

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

	/* Power off drops them. */
	ap_batch(PROTO_SNS_ACCEL, 20000000, 1000000000);
	ap_activate(PROTO_SNS_ACCEL, true, 0);
	CHECK(accel_shared);
	mode_message(374, NULL, 0, &a);
	CHECK(a.mode == PROTO_MODE_POWER_OFF && !accel_shared);
	memset(&got, 0, sizeof got);
	ap_sensors(2000);
	CHECK(got.reports == 0);

	/* The AP's boot drops them too. */
	ap_activate(PROTO_SNS_ACCEL, true, 0);
	CHECK(accel_shared);
	ap_sensors(1);
	ap_write(1, PROTO_BOOT, NULL, 0);
	companion_pass();
	CHECK(!accel_shared);
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

	if (failures)
		printf("companion_test: %d failures\n", failures);
	return failures != 0;
}
