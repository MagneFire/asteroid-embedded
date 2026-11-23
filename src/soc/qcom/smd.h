/* An SMD channel to a processor other than the RPM, in byte access:
 * each side has a half channel with its state, its signals and the
 * indexes into the FIFO it writes, and reads the other side's FIFO. A
 * packet channel puts a five-word header, the length first, before each
 * packet. No hardware involved: the caller tells the other side. */
#ifndef SMD_H
#define SMD_H
#include <stdbool.h>
#include <stdint.h>

#define SMD_CLOSED	0
#define SMD_OPENING	1
#define SMD_OPENED	2
#define SMD_FLUSHING	3
#define SMD_CLOSING	4
#define SMD_RESET	5

#define SMD_HEADER	20

struct smd_half {
	uint32_t state;
	uint8_t dsr, cts, cd, ri, head_changed, tail_changed, state_changed, block_read_intr;
	uint32_t tail, head;
};

struct smd_chan {
	volatile struct smd_half *send, *recv;
	volatile uint8_t *tx, *rx;
	uint32_t fifo;
};

uint32_t smd_read_avail(const struct smd_chan *c);
uint32_t smd_write_avail(const struct smd_chan *c);
/* Our side's state, with the signals that go with it. */
void smd_set_state(struct smd_chan *c, uint32_t state);
/* 0, or -1 without room for the header and all of it. */
int smd_packet_write(struct smd_chan *c, const void *data, uint32_t len);
/* A whole packet's length when one is there, 0 if not yet, -1 for one
 * longer than max (dropped) or a FIFO that makes no sense. */
int smd_packet_read(struct smd_chan *c, void *buf, uint32_t max);
#endif
