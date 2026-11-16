#include <string.h>
#include "wire.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void put16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

uint16_t wire_sum(const uint8_t *p, unsigned n)
{
	uint32_t sum = 0;
	unsigned i = 0;
	for (; i + 1 < n; i += 2)
		sum += le16(p + i);
	if (i < n)
		sum += p[i];
	if (sum >= 0x10000)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

void wire_header_put(uint8_t *h, uint16_t seq, uint8_t flags, const uint8_t *payload, unsigned len)
{
	put16(h, seq);
	h[2] = flags;
	h[3] = 0;
	put16(h + 4, wire_sum(payload, len));
	put16(h + 6, len);
	unsigned sum = 0;
	for (unsigned i = 0; i < WIRE_HEADER; i++)
		sum += h[i];
	h[3] = (uint8_t)(0xff - (sum & 0xff));
}

enum wire_error wire_header_get(const uint8_t *h, struct wire_header *out)
{
	unsigned sum = 0;
	for (unsigned i = 0; i < WIRE_HEADER; i++)
		sum += h[i];
	if ((sum & 0xff) != 0xff)
		return WIRE_BAD_HEADER;
	out->seq = le16(h);
	out->flags = h[2];
	out->sum = le16(h + 4);
	out->len = le16(h + 6);
	return out->len > WIRE_PAYLOAD_MAX ? WIRE_TOO_LONG : WIRE_OK;
}

unsigned wire_write_parts(unsigned len) { return (len + WIRE_HEADER + WIRE_WRITE_PART - 1) / WIRE_WRITE_PART; }
unsigned wire_read_parts(unsigned len) { return (len + WIRE_HEADER + WIRE_READ_PART - 1) / WIRE_READ_PART; }

void wire_rx_start(struct wire_rx *r)
{
	r->got = r->want = 0;
}

enum wire_rx_state wire_rx_part(struct wire_rx *r, const uint8_t *part, enum wire_error *err)
{
	if (!r->got) {
		enum wire_error e = wire_header_get(part, &r->h);
		if (e != WIRE_OK) {
			*err = e;
			return WIRE_RX_BAD;
		}
		r->want = WIRE_HEADER + r->h.len;
	}
	unsigned n = r->want - r->got < WIRE_WRITE_PART ? r->want - r->got : WIRE_WRITE_PART;
	memcpy(r->buf + r->got, part, n);
	r->got += n;
	if (r->got < r->want)
		return WIRE_RX_MORE;
	if (wire_sum(r->buf + WIRE_HEADER, r->h.len) != r->h.sum) {
		*err = WIRE_BAD_SUM;
		return WIRE_RX_BAD;
	}
	*err = WIRE_OK;
	return WIRE_RX_DONE;
}

void wire_read_slice(unsigned total, unsigned n, unsigned *off, unsigned *direct, unsigned *fifo)
{
	unsigned start = n * WIRE_READ_PART, left = start < total ? total - start : 0;
	*off = start;
	*direct = left < WIRE_READ_DIRECT ? left : WIRE_READ_DIRECT;
	left -= *direct;
	*fifo = left < WIRE_READ_FIFO ? left : WIRE_READ_FIFO;
}

bool wire_seq_take(struct wire_seq *s, const struct wire_header *h)
{
	if (h->seq != s->expect && !(h->flags & WIRE_FLAG_RESYNC) && !s->resync) {
		s->expect = h->seq;
		return false;
	}
	s->expect = (uint16_t)(h->seq + 1);
	s->resync = false;
	return true;
}

static bool raw_channel(uint16_t chan)
{
	return (chan >= WIRE_CHAN_FACTORY && chan <= WIRE_CHAN_BATCH) || chan == WIRE_CHAN_IGNORED ||
	       chan == WIRE_CHAN_MARK;
}

bool wire_unwrap(const uint8_t *p, unsigned n, struct wire_msg *m)
{
	if (n < 4)
		return false;
	unsigned len = le16(p + 2);
	if (len > n - 4)
		return false;
	m->chan = le16(p);
	if (raw_channel(m->chan)) {
		*m = (struct wire_msg){ m->chan, 0, (uint16_t)len, false, p + 4 };
		return true;
	}
	if (len < 4 || le16(p + 6) > len - 4)
		return false;
	*m = (struct wire_msg){ m->chan, le16(p + 4), le16(p + 6), true, p + 8 };
	return true;
}

unsigned wire_wrap(uint8_t *out, unsigned cap, uint16_t chan, uint16_t id, const uint8_t *body, unsigned len)
{
	if (len > WIRE_PAYLOAD_MAX - 8 || cap < 8 + len)
		return 0;
	put16(out, chan);
	put16(out + 2, 4 + len);
	put16(out + 4, id);
	put16(out + 6, len);
	memcpy(out + 8, body, len);
	return 8 + len;
}
