/* SMD packet channel FIFOs, with the other side played by the test. */
#include <stdio.h>
#include <string.h>
#include "soc/qcom/smd.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define FIFO 64
static struct smd_half half[2];
static uint8_t fifo[2 * FIFO];

/* The other side writes a packet into our receive FIFO. */
static void remote_write(const void *data, uint32_t len)
{
	uint32_t hdr[5] = { len }, head = half[1].head;
	const uint8_t *parts[2] = { (const uint8_t *)hdr, data };
	uint32_t lens[2] = { 20, len };
	for (unsigned k = 0; k < 2; k++)
		for (uint32_t i = 0; i < lens[k]; i++) {
			fifo[FIFO + head] = parts[k][i];
			head = (head + 1) % FIFO;
		}
	half[1].head = head;
}

/* The other side reads a packet from our send FIFO. */
static int remote_read(uint8_t *buf)
{
	uint32_t tail = half[0].tail, hdr[5];
	for (unsigned i = 0; i < 20; i++)
		((uint8_t *)hdr)[i] = fifo[(tail + i) % FIFO];
	for (uint32_t i = 0; i < hdr[0]; i++)
		buf[i] = fifo[(tail + 20 + i) % FIFO];
	half[0].tail = (tail + 20 + hdr[0]) % FIFO;
	return (int)hdr[0];
}

int main(void)
{
	struct smd_chan c = { &half[0], &half[1], fifo, fifo + FIFO, FIFO };
	uint8_t buf[64];

	CHECK(smd_read_avail(&c) == 0 && smd_write_avail(&c) == FIFO - 4);
	smd_set_state(&c, SMD_OPENING);
	CHECK(half[0].state == SMD_OPENING && !half[0].dsr && half[0].state_changed);
	smd_set_state(&c, SMD_OPENED);
	CHECK(half[0].state == SMD_OPENED && half[0].dsr && half[0].cts && half[0].cd);

	/* The 8-byte version request: header and all. */
	const uint32_t req[2] = { 0x01000000, 8 };
	CHECK(!smd_packet_write(&c, req, 8) && half[0].head == 28 && half[0].head_changed);
	uint32_t got[2];
	CHECK(remote_read((uint8_t *)got) == 8 && !memcmp(got, req, 8));
	/* 28 + 20 + 16 = 64 would fill it: four bytes stay free. */
	CHECK(smd_write_avail(&c) == FIFO - 4);
	CHECK(smd_packet_write(&c, buf, 41) == -1);
	/* Across the end of the FIFO. */
	for (unsigned i = 0; i < 30; i++)
		buf[i] = (uint8_t)(i + 1);
	CHECK(!smd_packet_write(&c, buf, 30) && half[0].head == (28 + 50) % FIFO);
	uint8_t back[64];
	CHECK(remote_read(back) == 30 && !memcmp(back, buf, 30));

	/* A reply, also across the end. */
	CHECK(smd_packet_read(&c, buf, sizeof buf) == 0);
	half[1].head = half[1].tail = 50;
	const uint8_t rsp[12] = { 1, 0, 0, 1, 12, 0, 0, 0, 1, 2, 3, 4 };
	remote_write(rsp, 12);
	CHECK(smd_read_avail(&c) == 32);
	memset(buf, 0, sizeof buf);
	CHECK(smd_packet_read(&c, buf, sizeof buf) == 12 && !memcmp(buf, rsp, 12));
	CHECK(half[1].tail == (50 + 32) % FIFO && half[0].tail_changed && smd_read_avail(&c) == 0);
	/* Only part of a packet there yet. */
	remote_write(rsp, 12);
	uint32_t whole = half[1].head;
	half[1].head = (half[1].tail + 24) % FIFO;
	CHECK(smd_packet_read(&c, buf, sizeof buf) == 0);
	half[1].head = whole;
	/* Too long for the buffer: dropped. */
	CHECK(smd_packet_read(&c, buf, 4) == -1 && smd_read_avail(&c) == 0);
	/* A length that could never fit in the FIFO. */
	uint32_t big[5] = { FIFO - 20 - 3 };
	for (unsigned i = 0; i < 20; i++)
		fifo[FIFO + (half[1].head + i) % FIFO] = ((uint8_t *)big)[i];
	half[1].head = (half[1].head + 20) % FIFO;
	CHECK(smd_packet_read(&c, buf, sizeof buf) == -1);
	half[1].tail = half[1].head;
	/* The other side's flags, and whether it wants a read kick. */
	half[1].head_changed = half[1].tail_changed = half[1].state_changed = 1;
	smd_flags_seen(&c);
	CHECK(!half[1].head_changed && !half[1].tail_changed && !half[1].state_changed);
	CHECK(smd_read_kick_wanted(&c));
	half[1].block_read_intr = 1;
	CHECK(!smd_read_kick_wanted(&c));
	/* Indexes past the FIFO. */
	half[1].head = FIFO;
	CHECK(smd_read_avail(&c) == 0);

	if (failures)
		printf("%d failures\n", failures);
	else
		printf("smd: all passed\n");
	return failures != 0;
}
