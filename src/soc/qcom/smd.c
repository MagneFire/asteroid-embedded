/* As the vendor kernel keeps it: a writer leaves 4 bytes free so that
 * a full FIFO is not taken for an empty one, and after moving an index
 * sets the matching changed flag in its own half. */
#include "asteroid.h"
#include "smd.h"

#define FULL_RESERVE	4

uint32_t smd_read_avail(const struct smd_chan *c)
{
	uint32_t head = c->recv->head, tail = c->recv->tail;
	if (head >= c->fifo || tail >= c->fifo)
		return 0;
	return head >= tail ? head - tail : head + c->fifo - tail;
}

uint32_t smd_write_avail(const struct smd_chan *c)
{
	uint32_t head = c->send->head, tail = c->send->tail;
	if (head >= c->fifo || tail >= c->fifo)
		return 0;
	uint32_t n = tail > head ? tail - head : tail + c->fifo - head;
	return n > FULL_RESERVE ? n - FULL_RESERVE : 0;
}

void smd_set_state(struct smd_chan *c, uint32_t state)
{
	uint8_t on = state == SMD_OPENED;
	c->send->dsr = c->send->cts = c->send->cd = on;
	c->send->state = state;
	dmb();
	c->send->state_changed = 1;
}

static void put(struct smd_chan *c, const uint8_t *p, uint32_t n)
{
	uint32_t head = c->send->head;
	for (uint32_t i = 0; i < n; i++) {
		c->tx[head] = p[i];
		if (++head == c->fifo)
			head = 0;
	}
	dmb();
	c->send->head = head;
}

int smd_packet_write(struct smd_chan *c, const void *data, uint32_t len)
{
	if (smd_write_avail(c) < SMD_HEADER + len)
		return -1;
	uint32_t hdr[SMD_HEADER / 4] = { len };
	put(c, (const uint8_t *)hdr, SMD_HEADER);
	put(c, data, len);
	dmb();
	c->send->head_changed = 1;
	return 0;
}

static void get(struct smd_chan *c, uint32_t at, uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (p)
			p[i] = c->rx[at];
		if (++at == c->fifo)
			at = 0;
	}
}

int smd_packet_read(struct smd_chan *c, void *buf, uint32_t max)
{
	uint32_t avail = smd_read_avail(c), tail = c->recv->tail;
	if (avail < SMD_HEADER)
		return 0;
	uint32_t hdr[SMD_HEADER / 4];
	get(c, tail, (uint8_t *)hdr, SMD_HEADER);
	uint32_t len = hdr[0];
	if (len > c->fifo)
		return -1;
	if (avail < SMD_HEADER + len)
		return 0;
	get(c, (tail + SMD_HEADER) % c->fifo, len <= max ? buf : NULL, len);
	dmb();
	c->recv->tail = (tail + SMD_HEADER + len) % c->fifo;
	dmb();
	c->send->tail_changed = 1;
	return len <= max ? (int)len : -1;
}
