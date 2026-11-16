/* The framing of the link between beluga's application processor and
 * its Apollo3, no hardware involved. A packet is an 8-byte header and
 * its payload: the sequence number (little endian), flags, a byte that
 * makes the header's bytes add up to 0xff, the payload's checksum and
 * its length. The AP writes a packet in parts of 120 bytes, the header
 * in the first; it reads one in parts of 248, each from a 256-byte read
 * that keeps bytes 0-119 and 126-253. The payload starts with the
 * kernel's envelope, a channel and a length; on the framework's
 * channels a message id and the body's length come next. */
#ifndef COMPANION_WIRE_H
#define COMPANION_WIRE_H
#include <stdbool.h>
#include <stdint.h>

#define WIRE_HEADER		8u
#define WIRE_PAYLOAD_MAX	520u
#define WIRE_FRAME_MAX		(WIRE_HEADER + WIRE_PAYLOAD_MAX)
#define WIRE_WRITE_PART		120u
#define WIRE_READ_DIRECT	120u
#define WIRE_READ_FIFO		128u
#define WIRE_READ_PART		(WIRE_READ_DIRECT + WIRE_READ_FIFO)
/* Lets the receiver take any sequence number. */
#define WIRE_FLAG_RESYNC	0x01

#define WIRE_CHAN_DATA		1
#define WIRE_CHAN_FACTORY	2
#define WIRE_CHAN_GPS		3
#define WIRE_CHAN_SENSORS	4
#define WIRE_CHAN_BATCH		5
#define WIRE_CHAN_IGNORED	3241
#define WIRE_CHAN_MARK		3240

struct wire_header {
	uint16_t seq, sum, len;
	uint8_t flags;
};

/* The sum of the little-endian 16-bit words, an odd last byte as the
 * low byte of one, its carry folded in once, inverted. */
uint16_t wire_sum(const uint8_t *p, unsigned n);
/* The header for a payload of len bytes. */
void wire_header_put(uint8_t *h, uint16_t seq, uint8_t flags, const uint8_t *payload, unsigned len);

enum wire_error { WIRE_OK, WIRE_BAD_HEADER, WIRE_TOO_LONG, WIRE_BAD_SUM, WIRE_BAD_SEQ };
enum wire_error wire_header_get(const uint8_t *h, struct wire_header *out);

/* How many parts a payload of len bytes takes each way. */
unsigned wire_write_parts(unsigned len);
unsigned wire_read_parts(unsigned len);

/* A packet the AP writes, put together from its parts. */
struct wire_rx {
	uint8_t buf[WIRE_FRAME_MAX];
	unsigned got, want;
	struct wire_header h;
};
enum wire_rx_state { WIRE_RX_MORE, WIRE_RX_DONE, WIRE_RX_BAD };
void wire_rx_start(struct wire_rx *r);
/* One part, WIRE_WRITE_PART bytes; *err says why it went bad. */
enum wire_rx_state wire_rx_part(struct wire_rx *r, const uint8_t *part, enum wire_error *err);

/* For read part n of a frame of total bytes: where in the frame its
 * direct bytes start and how many there are of them and of the FIFO's
 * bytes, which follow them in the frame. */
void wire_read_slice(unsigned total, unsigned n, unsigned *off, unsigned *direct, unsigned *fifo);

/* The receiver's sequence: one it does not expect is refused and
 * expected next, unless the header or a fresh start lets it through. */
struct wire_seq {
	uint16_t expect;
	bool resync;
};
bool wire_seq_take(struct wire_seq *s, const struct wire_header *h);

/* A payload's envelope; body is what follows the message id and length
 * on the framework's channels, and everything after the channel and
 * length on the others (framework false, id 0). */
struct wire_msg {
	uint16_t chan, id, len;
	bool framework;
	const uint8_t *body;
};
bool wire_unwrap(const uint8_t *p, unsigned n, struct wire_msg *m);
/* The channel for an answer to a message on chan: one of the
 * framework's two. */
uint16_t wire_reply_chan(uint16_t chan);
/* A framework message in its envelope; 0 if it does not fit. */
unsigned wire_wrap(uint8_t *out, unsigned cap, uint16_t chan, uint16_t id, const uint8_t *body, unsigned len);
#endif
