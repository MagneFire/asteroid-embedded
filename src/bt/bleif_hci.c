/* HCI over the Apollo3's BLE interface. Each read from the controller is
 * one whole packet, so there is no byte parser: packets wait in a few
 * slots until the main loop hands them on, and a write the controller
 * refuses because it wants to send first can go out once its packets
 * are in. Command complete and command status events for the command
 * being waited on end hci_cmd, everything else goes to the handler. */
#include <string.h>
#include "asteroid.h"
#include "bt.h"
#include "bleif.h"
#include "bleif_proto.h"

#define SLOTS		4
#define SEND_MS		100

static uint32_t slot[SLOTS][BLEIF_PACKET_WORDS], txbuf[BLEIF_PACKET_WORDS];
static uint16_t slot_len[SLOTS];
static unsigned head, tail;
static bool delivering;
static hci_packet_fn handler;
static uint32_t packets, errors;

static struct {
	uint16_t opcode;
	volatile bool done;
	uint8_t *rsp;
	unsigned max, len;
	int status;
} waiting;

static void fetch(void)
{
	for (unsigned i = 0; i < 2 * SLOTS && tail - head < SLOTS && bleif_link_pending(); i++) {
		uint8_t *p = (uint8_t *)slot[tail % SLOTS];
		int n = bleif_link_read(slot[tail % SLOTS], sizeof slot[0]);
		if (n <= 0) {
			if (n < 0)
				errors++;
			return;
		}
		if (!bleif_packet_ok(p, (unsigned)n)) {
			errors++;
			continue;
		}
		slot_len[tail % SLOTS] = (uint16_t)n;
		tail++;
	}
}

static void deliver(uint8_t *p, unsigned n)
{
	uint8_t type = *p++;
	n--;
	packets++;
	if (type == HCI_EVENT) {
		bleif_fix_event(p, n);
		if (waiting.opcode && !waiting.done) {
			if (p[0] == HCI_EV_COMMAND_COMPLETE && p[1] >= 3 && (p[3] | p[4] << 8) == waiting.opcode) {
				unsigned k = p[1] - 3u;
				waiting.status = k ? p[5] : 0;
				waiting.len = k < waiting.max ? k : waiting.max;
				if (waiting.rsp)
					memcpy(waiting.rsp, p + 5, waiting.len);
				waiting.done = true;
				return;
			}
			if (p[0] == HCI_EV_COMMAND_STATUS && p[1] >= 4 && (p[4] | p[5] << 8) == waiting.opcode) {
				waiting.status = p[2];
				waiting.len = 0;
				waiting.done = true;
				return;
			}
		}
	}
	if (handler)
		handler(type, p, n);
}

/* A handler that waits on the controller only gets its packets read, so
 * the one being handed on keeps its slot. */
bool bleif_hci_service(void)
{
	fetch();
	if (delivering)
		return false;
	bool any = false;
	delivering = true;
	while (head != tail) {
		deliver((uint8_t *)slot[head % SLOTS], slot_len[head % SLOTS]);
		head++;
		any = true;
		fetch();
	}
	delivering = false;
	return any;
}

void hci_link_wakes(uint32_t *irqs, uint32_t *empty) { *irqs = *empty = 0; }

bool bleif_hci_idle(void)
{
	return head == tail;
}

void hci_reset_parser(void)
{
	head = tail = 0;
	waiting.opcode = 0;
}

void hci_set_handler(hci_packet_fn fn)
{
	handler = fn;
}

void hci_poll(void)
{
	bleif_hci_service();
}

int hci_send(uint8_t type, const uint8_t *p, unsigned n)
{
	if (n + 1 > sizeof txbuf)
		return -1;
	uint8_t *b = (uint8_t *)txbuf;
	b[0] = type;
	memcpy(b + 1, p, n);
	uint32_t t0 = time_ms();
	while (bleif_link_write(txbuf, n + 1)) {
		fetch();
		if (time_ms() - t0 >= SEND_MS) {
			errors++;
			return -1;
		}
		wdt_feed();
	}
	return 0;
}

int hci_cmd(uint16_t opcode, const void *param, uint8_t plen, uint8_t *rsp, unsigned *rsp_len, uint32_t timeout_ms)
{
	uint8_t c[3 + 255];
	c[0] = (uint8_t)opcode;
	c[1] = (uint8_t)(opcode >> 8);
	c[2] = plen;
	if (plen)
		memcpy(c + 3, param, plen);
	waiting.opcode = opcode;
	waiting.done = false;
	waiting.rsp = rsp;
	waiting.max = rsp_len ? *rsp_len : 0;
	waiting.len = 0;
	waiting.status = 0;
	if (hci_send(HCI_COMMAND, c, 3u + plen)) {
		waiting.opcode = 0;
		return -1;
	}
	uint32_t t0 = time_ms();
	while (!waiting.done) {
		hci_poll();
		wdt_feed();
		if (!waiting.done && time_ms() - t0 > timeout_ms) {
			waiting.opcode = 0;
			return -2;
		}
	}
	waiting.opcode = 0;
	if (rsp_len)
		*rsp_len = waiting.len;
	return waiting.status;
}

void hci_stats(uint32_t *p, uint32_t *e)
{
	*p = packets;
	*e = errors;
}
