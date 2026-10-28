/* HCI over the H4 UART transport: a packet type byte, then the packet
 * (event: code, length; ACL: handle, 16-bit length; SCO: handle, length).
 * The receive side is a byte parser over the UART ring, run from the
 * main loop; command complete and command status events for the command
 * being waited on end hci_cmd, everything else goes to the handler. */
#include <string.h>
#include "asteroid.h"
#include "bt.h"

#define MAX_PACKET	(4 + 1024)

static uint8_t pkt[MAX_PACKET], txbuf[1 + MAX_PACKET];
static uint8_t type;
static unsigned have, need;
static bool payload;
static hci_packet_fn handler;
static uint32_t packets, errors;

static struct {
	uint16_t opcode;
	volatile bool done;
	uint8_t *rsp;
	unsigned max, len;
	int status;
} waiting;

static unsigned header_len(uint8_t t)
{
	return t == HCI_EVENT ? 2 : t == HCI_ACL ? 4 : t == HCI_SCO ? 3 : 0;
}

static unsigned payload_len(void)
{
	return type == HCI_EVENT ? pkt[1] : type == HCI_ACL ? (unsigned)(pkt[2] | pkt[3] << 8) : pkt[2];
}

static void deliver(void)
{
	packets++;
	if (type == HCI_EVENT && waiting.opcode && !waiting.done) {
		if (pkt[0] == HCI_EV_COMMAND_COMPLETE && pkt[1] >= 3 && (pkt[3] | pkt[4] << 8) == waiting.opcode) {
			unsigned n = pkt[1] - 3u;
			waiting.status = n ? pkt[5] : 0;
			waiting.len = n < waiting.max ? n : waiting.max;
			if (waiting.rsp)
				memcpy(waiting.rsp, pkt + 5, waiting.len);
			waiting.done = true;
			return;
		}
		if (pkt[0] == HCI_EV_COMMAND_STATUS && pkt[1] >= 4 && (pkt[4] | pkt[5] << 8) == waiting.opcode) {
			waiting.status = pkt[2];
			waiting.len = 0;
			waiting.done = true;
			return;
		}
	}
	if (handler)
		handler(type, pkt, have);
}

static void feed(uint8_t c)
{
	if (!type) {
		need = header_len(c);
		if (!need) {
			errors++;
			return;
		}
		type = c;
		have = 0;
		payload = false;
		return;
	}
	pkt[have++] = c;
	if (have < need)
		return;
	if (!payload) {
		payload = true;
		need += payload_len();
		if (need > sizeof pkt) {
			errors++;
			type = 0;
			return;
		}
		if (have < need)
			return;
	}
	deliver();
	type = 0;
}

void hci_reset_parser(void)
{
	type = 0;
	have = need = 0;
	waiting.opcode = 0;
}

void hci_set_handler(hci_packet_fn fn)
{
	handler = fn;
}

void hci_poll(void)
{
	uint8_t buf[64];
	unsigned n;
	while ((n = uart_dm_read(buf, sizeof buf)))
		for (unsigned i = 0; i < n; i++)
			feed(buf[i]);
}

int hci_send(uint8_t t, const uint8_t *p, unsigned n)
{
	if (n > MAX_PACKET)
		return -1;
	txbuf[0] = t;
	memcpy(txbuf + 1, p, n);
	return uart_dm_write(txbuf, n + 1);
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
