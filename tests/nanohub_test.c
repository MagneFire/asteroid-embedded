/* The sensor hub's packet framing: the byte-exact packets of the research notes, decoded and rebuilt. */
#include <stdio.h>
#include <string.h>
#include "sensors/nanohub_proto.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static unsigned hex(const char *s, uint8_t *out)
{
	unsigned n = 0;
	for (; *s; s++) {
		if (*s == ' ')
			continue;
		unsigned v;
		sscanf(s, "%2x", &v);
		out[n++] = (uint8_t)v;
		s++;
	}
	return n;
}

/* Builds the packet from its parts, and checks the bytes are the example's. */
static void vector(const char *name, uint32_t seq, uint32_t reason, const uint8_t *payload, unsigned len,
		   const char *bytes)
{
	uint8_t want[NANOHUB_PACKET_MAX], got[NANOHUB_PACKET_MAX];
	unsigned n = hex(bytes, want);
	unsigned m = nanohub_encode(got, seq, reason, payload, len);
	if (m != n || memcmp(got, want, n)) {
		printf("%s: encode differs\n", name);
		failures++;
	}
	struct nanohub_packet p;
	unsigned used;
	CHECK(nanohub_decode(want, n, &p, &used) == NANOHUB_OK);
	CHECK(used == n && p.seq == seq && p.reason == reason && p.len == len && !memcmp(p.payload, payload, len));
}

static void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

int main(void)
{
	uint8_t pl[NANOHUB_PAYLOAD_MAX], buf[NANOHUB_PACKET_MAX + 16];

	vector("get versions", 1, NANOHUB_GET_VERSIONS, NULL, 0, "31 01 00 00 00 00 10 00 00 00 8b 85 b1 b5");

	unsigned n = nanohub_read_payload(pl, 0x123456789ull);
	CHECK(n == 8);
	vector("read", 2, NANOHUB_READ, pl, n, "31 02 00 00 00 90 10 00 00 08 89 67 45 23 01 00 00 00 29 df ef 7d");

	memset(pl, 0, 32);
	pl[0] = 6;
	vector("clear interrupts", 3, NANOHUB_CLR_GET_INTR, pl, 32,
	       "31 03 00 00 00 80 10 00 00 20 06 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
	       "00 00 00 00 00 00 aa 9a 78 16");

	pl[0] = 2;
	vector("mask interrupt", 7, NANOHUB_MASK_INTR, pl, 1, "31 07 00 00 00 81 10 00 00 01 02 79 cd da d4");

	n = nanohub_sensor_config(pl, NANOHUB_SENSOR_ACCEL, NANOHUB_CMD_ENABLE, NANOHUB_HZ(50), 0, NULL, 0);
	CHECK(n == 20 && NANOHUB_HZ(50) == 0xc800);
	vector("accelerometer", 4, NANOHUB_WRITE, pl, n,
	       "31 04 00 00 00 91 10 00 00 14 00 03 00 00 00 00 00 00 00 00 00 00 00 c8 00 00 01 01 00 00 d3 39 6e a8");

	n = nanohub_sensor_config(pl, NANOHUB_SENSOR_STEP_COUNTER, NANOHUB_CMD_ENABLE, NANOHUB_RATE_ON_CHANGE, 0, NULL, 0);
	vector("step counter", 5, NANOHUB_WRITE, pl, n,
	       "31 05 00 00 00 91 10 00 00 14 00 03 00 00 00 00 00 00 00 00 00 00 01 ff ff ff 16 01 00 00 7b 71 a6 82");

	n = nanohub_time_config(pl, 1767225600, 3600, true);
	CHECK(n == 36);
	vector("time", 6, NANOHUB_WRITE, pl, n,
	       "31 06 00 00 00 91 10 00 00 24 00 03 00 00 00 00 00 00 00 00 00 00 00 00 00 00 55 03 00 00 00 b9 55 69 00 00 "
	       "00 00 10 0e 00 00 01 00 00 00 61 0b 76 55");
	/* A zone west of Greenwich is a negative offset. */
	n = nanohub_time_config(pl, 1, -18000, false);
	CHECK(pl[28] == 0xb0 && pl[29] == 0xb9 && pl[30] == 0xff && pl[31] == 0xff && pl[32] == 0);

	/* The ACK to seq 2 with nothing pending. */
	memset(pl, 0, 32);
	uint8_t ack[NANOHUB_PACKET_MAX];
	unsigned alen = hex("31 02 00 00 00 00 00 00 00 20", ack);
	memset(ack + alen, 0, 32);
	alen += 32;
	alen += hex("c4 ab 06 51", ack + alen);
	vector("ack", 2, NANOHUB_ACK, pl, 32, "31 02 00 00 00 00 00 00 00 20 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
	       "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 c4 ab 06 51");

	/* Behind filler, as the hub clocks it: 0xff before, and after. */
	struct nanohub_packet p;
	unsigned used;
	memset(buf, 0xff, sizeof(buf));
	memcpy(buf + 7, ack, alen);
	CHECK(nanohub_decode(buf, 7 + alen + 5, &p, &used) == NANOHUB_OK);
	CHECK(used == 7 + alen && p.reason == NANOHUB_ACK && p.seq == 2 && nanohub_ack_intr(&p) == 0);
	/* Zeros are filler too. */
	buf[0] = 0;
	CHECK(nanohub_decode(buf, 7 + alen, &p, &used) == NANOHUB_OK);
	pl[0] = 0x0d;
	nanohub_encode(buf, 9, NANOHUB_ACK, pl, 32);
	CHECK(nanohub_decode(buf, NANOHUB_HEADER + 36, &p, &used) == NANOHUB_OK && nanohub_ack_intr(&p) == 0x0d);
	/* Only an ACK has an interrupt word; the answer to a READ of 32 bytes does not. */
	nanohub_encode(buf, 9, NANOHUB_READ, pl, 32);
	CHECK(nanohub_decode(buf, NANOHUB_HEADER + 36, &p, &used) == NANOHUB_OK && !nanohub_ack_intr(&p));

	/* Not all there yet: nothing is used. */
	CHECK(nanohub_decode(ack, 0, &p, &used) == NANOHUB_NEED_MORE && used == 0);
	CHECK(nanohub_decode(buf + 100, 0, &p, &used) == NANOHUB_NEED_MORE);
	memset(buf, 0xff, 40);
	CHECK(nanohub_decode(buf, 40, &p, &used) == NANOHUB_NEED_MORE && used == 0);
	CHECK(nanohub_decode(ack, 5, &p, &used) == NANOHUB_NEED_MORE && used == 0);
	CHECK(nanohub_decode(ack, 9, &p, &used) == NANOHUB_NEED_MORE);
	CHECK(nanohub_decode(ack, 10, &p, &used) == NANOHUB_NEED_MORE);
	CHECK(nanohub_decode(ack, alen - 1, &p, &used) == NANOHUB_NEED_MORE && used == 0);
	/* A flipped bit, a wrong sync byte. */
	memcpy(buf, ack, alen);
	buf[20] ^= 1;
	CHECK(nanohub_decode(buf, alen, &p, &used) == NANOHUB_BAD && used == 1);
	memcpy(buf, ack, alen);
	buf[alen - 1] ^= 0x80;
	CHECK(nanohub_decode(buf, alen, &p, &used) == NANOHUB_BAD);
	memcpy(buf, ack, alen);
	buf[0] = 0x32;
	CHECK(nanohub_decode(buf, alen, &p, &used) == NANOHUB_BAD && used == 1);
	/* Two in a row: the first is used up and the second remains. */
	memcpy(buf, ack, alen);
	memcpy(buf + alen, ack, alen);
	CHECK(nanohub_decode(buf, 2 * alen, &p, &used) == NANOHUB_OK && used == alen);
	CHECK(nanohub_decode(buf + used, 2 * alen - used, &p, &used) == NANOHUB_OK && used == alen);

	/* Limits. */
	CHECK(nanohub_encode(buf, 1, 1, pl, 256) == 0);
	CHECK(nanohub_encode(buf, 1, 1, pl, 255) == NANOHUB_PACKET_MAX);
	CHECK(nanohub_decode(buf, NANOHUB_PACKET_MAX, &p, &used) == NANOHUB_OK && p.len == 255);
	CHECK(nanohub_sensor_config(pl, 1, 3, 0, 0, buf, 236) == 0 && nanohub_sensor_config(pl, 1, 3, 0, 0, buf, 235) == 255);
	/* A payload of every length survives the padding of the last CRC word. */
	for (unsigned len = 0; len < 12; len++) {
		uint8_t src[12], out[NANOHUB_PACKET_MAX];
		for (unsigned i = 0; i < 12; i++)
			src[i] = (uint8_t)(0x40 + i);
		unsigned m = nanohub_encode(out, len, 0x1234, src, len);
		CHECK(m == 14 + len && nanohub_decode(out, m, &p, &used) == NANOHUB_OK && p.len == len);
	}
	/* Padding is part of the CRC: a short tail and the same bytes with zeros after it agree. */
	CHECK(nanohub_crc32((const uint8_t *)"\x01\x02\x03", 3) == nanohub_crc32((const uint8_t *)"\x01\x02\x03\x00", 4));

	/* Events. Nothing pending, too short. */
	struct nanohub_event ev;
	CHECK(nanohub_event(NULL, 0, &ev) && ev.kind == NANOHUB_EV_EMPTY);
	CHECK(!nanohub_event(pl, 3, &ev) && ev.kind == NANOHUB_EV_BAD);

	/* A step counter event: type 0x216, hub time, one sample of a count. The top bit of the type is ignored. */
	uint8_t e[64];
	memset(e, 0, sizeof(e));
	put32(e, 0x80000216);
	put32(e + 4, 1000);
	put32(e + 8, 2);
	e[12] = 1;
	put32(e + 16, 4242);
	CHECK(nanohub_event(e, 20, &ev) && ev.kind == NANOHUB_EV_SENSOR && ev.type == 0x216);
	CHECK(ev.sensor.type == NANOHUB_SENSOR_STEP_COUNTER && ev.sensor.header && !ev.sensor.wake);
	CHECK(ev.sensor.ref_ns == (2ull << 32 | 1000) && ev.sensor.samples == 1 && ev.sensor.size == 8);
	uint64_t ns;
	const uint8_t *d;
	CHECK(nanohub_sample(&ev, 0, 4, &ns, &d) && ns == ev.sensor.ref_ns && d[0] == 0x92 && d[1] == 0x10);
	CHECK(!nanohub_sample(&ev, 1, 4, &ns, &d));

	/* Three accelerometer samples, raw int16, 20 ms (39062 << 9 ns, the low bit is a flag) between. */
	memset(e, 0, sizeof(e));
	put32(e, 0x201);
	put32(e + 4, 5000);
	e[12] = 3;
	e[15] = 1;
	e[16] = 1;
	put32(e + 22, 39062u | 1);
	e[26] = 2;
	put32(e + 32, 39062);
	e[36] = 3;
	CHECK(nanohub_event(e, 42, &ev) && ev.sensor.type == NANOHUB_SENSOR_ACCEL && ev.sensor.wake && ev.sensor.samples == 3);
	CHECK(nanohub_sample(&ev, 0, 6, &ns, &d) && ns == 5000 && d[0] == 1);
	CHECK(nanohub_sample(&ev, 1, 6, &ns, &d) && ns == 5000 + ((39062ull & ~1ull) << 9) && d[0] == 2);
	CHECK(nanohub_sample(&ev, 2, 6, &ns, &d) && ns == 5000 + 2 * ((39062ull & ~1ull) << 9) && d[0] == 3);
	CHECK(!nanohub_sample(&ev, 3, 6, &ns, &d));
	CHECK(!nanohub_sample(&ev, 2, 7, &ns, &d));

	/* The hand-up gesture is only the event. */
	put32(e, 0x24b);
	CHECK(nanohub_event(e, 4, &ev) && ev.kind == NANOHUB_EV_SENSOR && ev.sensor.type == NANOHUB_SENSOR_HAND_UP && !ev.sensor.header);
	CHECK(!nanohub_sample(&ev, 0, 4, &ns, &d));

	/* A log line. */
	put32(e, 0x3b474f4c);
	e[4] = 'I';
	memcpy(e + 5, "Started 20 internal apps", 24);
	CHECK(nanohub_event(e, 29, &ev) && ev.kind == NANOHUB_EV_LOG && ev.log.level == 'I' && ev.log.size == 24);
	CHECK(!memcmp(ev.log.text, "Started", 7));

	/* The fuel gauge: the readings of the notes' journal of a full battery, 32.9 degC, 4429 mV, 15 mA. */
	uint8_t g[NANOHUB_FUEL_GAUGE_SIZE];
	memset(g, 0, sizeof(g));
	put32(g, 0x401);
	/* The app id: "Mobvo" in the top five bytes, 4 in the low. */
	put32(g + 4, 0x6f000004);
	put32(g + 8, 0x4d6f6276);
	g[12] = 43;
	g[13] = 0xfe;
	g[14] = 68;
	g[15] = 0;
	uint8_t *c = g + 16;
	c[0] = 4;
	c[1] = 1;
	c[2] = 0x8a;
	c[3] = 0x22;
	c[4] = 329 & 0xff;
	c[5] = 329 >> 8;
	c[6] = 4429 & 0xff;
	c[7] = 4429 >> 8;
	c[8] = 0x08;
	c[9] = 0x02;
	c[16] = 0xf1;
	c[17] = 0xff;
	c[30] = 100;
	c[32] = 66;
	c[34] = 1;
	c[36] = 415 & 0xff;
	c[37] = 415 >> 8;
	CHECK(nanohub_event(g, sizeof(g), &ev) && ev.kind == NANOHUB_EV_FUEL_GAUGE);
	CHECK(ev.gauge.temp_dc == 329 && ev.gauge.mv == 4429 && ev.gauge.percent == 100 && ev.gauge.current_ma == -15);
	CHECK(ev.gauge.flags == 0x0208 && ev.gauge.discharging == false && ev.gauge.full_charge == true);
	CHECK(ev.gauge.design_mah == 415 && ev.gauge.power_mw == 66 && ev.gauge.health == 1 && ev.gauge.control == 0x228a);
	CHECK(ev.gauge.status == 4 && ev.gauge.present == 1);
	/* Another app's message, and the gauge's with another size or status, are only app messages. */
	g[4] = 5;
	CHECK(nanohub_event(g, sizeof(g), &ev) && ev.kind == NANOHUB_EV_APP && ev.app.size == 43);
	g[4] = 4;
	g[15] = 1;
	CHECK(nanohub_event(g, sizeof(g), &ev) && ev.kind == NANOHUB_EV_APP);
	g[15] = 0;
	g[12] = 255;
	CHECK(!nanohub_event(g, sizeof(g), &ev));
	CHECK(!nanohub_event(g, 12, &ev));
	put32(g, 0x1234);
	CHECK(nanohub_event(g, sizeof(g), &ev) && ev.kind == NANOHUB_EV_OTHER && ev.type == 0x1234);

	/* The band-mode log: the read request as the research worked it out
	 * for sequence 10, a chunk with a record and the tag the hub writes
	 * at each start of band mode, and the reply to an erase. */
	uint8_t rq[2] = { 1, 1 };
	n = nanohub_sensor_config(pl, NANOHUB_SENSOR_CUSTOM_FLASH, NANOHUB_CMD_CFG_DATA, 0, 0, rq, sizeof rq);
	vector("log read", 10, NANOHUB_WRITE, pl, n,
	       "31 0a 00 00 00 91 10 00 00 16 00 03 00 00 00 00 00 00 00 00 00 00 00 00 00 00 59 03 00 00 01 01 a8 7f 2e e2");
	static const uint8_t chunk[] = {
		0x01, 0x04, 0x00, 0x00, 0x19, 0x00, 0x00, 0x6f, 0x76, 0x62, 0x6f, 0x4d, 5 + 24, 0xfe, 0x59, 0x00, 24, 1,
		0x10, 0x32, 0x54, 0x76, 0x2a, 0x00, 0x18, 0x48, /* time 0x76543210, 42 steps, type 3, 72 bpm */
		'F', 'L', 'A', 'S', 'H', '_', 'S', 'T', 'O', 'R', 'E', '_', 'T', 'A', 'G', 0,
	};
	struct nanohub_event lev;
	const uint8_t *rec;
	bool last = false;
	CHECK(nanohub_event(chunk, sizeof chunk, &lev) && lev.kind == NANOHUB_EV_APP);
	CHECK(nanohub_log_chunk(&lev, &rec, &last) == 24 && last && !nanohub_log_erased(&lev));
	struct nanohub_log_record r;
	CHECK(nanohub_log_record(rec, &r) && r.time == 0x76543210 && r.steps == 42 && r.type == 3 && r.bpm == 72);
	CHECK(!nanohub_log_record(rec + 8, &r) && !nanohub_log_record(rec + 16, &r));
	static const uint8_t erased[] = { 0x01, 0x04, 0x00, 0x00, 0x19, 0x00, 0x00, 0x6f, 0x76, 0x62, 0x6f, 0x4d, 4, 0xfe, 0x59, 0x00, 0x01 };
	CHECK(nanohub_event(erased, sizeof erased, &lev) && nanohub_log_erased(&lev) && nanohub_log_chunk(&lev, &rec, &last) < 0);

	puts(failures ? "nanohub_test: FAILED" : "nanohub_test: ok");
	return failures != 0;
}
