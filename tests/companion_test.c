/* The framing of beluga's AP link: the AP's side of it is played here
 * as deamonserver does it, its writes and its reads. */
#include <stdio.h>
#include <string.h>
#include "companion/wire.h"

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

int main(void)
{
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

	if (failures)
		printf("companion_test: %d failures\n", failures);
	return failures != 0;
}
