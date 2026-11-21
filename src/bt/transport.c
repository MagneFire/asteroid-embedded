/* NimBLE's controller-side transport over our H4 UART: commands and ACL
 * from the host go straight out with hci_send(), events and ACL from the
 * parser in hci.c are copied into the transport's buffers and handed to
 * the host. ACL flow control (LE Read Buffer Size, Number of Completed
 * Packets) is the host's own; this layer only moves packets. */
#include <string.h>
#include "asteroid.h"
#include "os/os_mbuf.h"
#include "nimble/hci_common.h"
#include "nimble/transport.h"
#include "bt.h"

static bool attached;
static uint8_t acl_buf[4 + 1024];
static struct bt_transport_stats stats;

/* What came in since the last asleep line: e for an event code and LE
 * subevent, a for an ATT opcode and handle, l for another L2CAP channel,
 * f for a continuation fragment. */
#define KINDS 6
static struct {
	uint8_t kind, code;
	uint16_t handle;
	uint32_t n;
} kinds[KINDS];
static uint32_t kinds_other, kinds_total, rx_bytes0;

static void count(uint8_t kind, uint8_t code, uint16_t handle)
{
	kinds_total++;
	for (unsigned i = 0; i < KINDS; i++) {
		if (kinds[i].n && (kinds[i].kind != kind || kinds[i].code != code || kinds[i].handle != handle))
			continue;
		kinds[i].kind = kind;
		kinds[i].code = code;
		kinds[i].handle = handle;
		kinds[i].n++;
		return;
	}
	kinds_other++;
}

/* Only after a stretch with many packets, so a quiet sleep logs nothing. */
void bt_transport_log(void)
{
	uint32_t rx, tx, err;
	uart_stats(BT_UART, &rx, &tx, &err);
	if (kinds_total >= 100) {
		char s[160];
		unsigned o = 0;
		for (unsigned i = 0; i < KINDS && kinds[i].n && o < sizeof s; i++)
			o += (unsigned)snprintf(s + o, sizeof s - o, ", %c %02x/%04x %lu", kinds[i].kind, kinds[i].code, kinds[i].handle,
						kinds[i].n);
		log_printf("bt: %lu packets in, %lu uart bytes%s, %lu other\n", kinds_total, rx - rx_bytes0, o ? s : "", kinds_other);
	}
	memset(kinds, 0, sizeof kinds);
	kinds_other = kinds_total = 0;
	rx_bytes0 = rx;
}

static void rx(uint8_t type, const uint8_t *p, unsigned n)
{
	if (!attached)
		return;
	if (type == HCI_EVENT) {
		bool discardable = n > 2 && p[0] == BLE_HCI_EVCODE_LE_META && p[2] == BLE_HCI_LE_SUBEV_ADV_RPT;
		uint8_t *buf = n <= MYNEWT_VAL(BLE_TRANSPORT_EVT_SIZE) ? ble_transport_alloc_evt(discardable) : NULL;
		if (!buf) {
			stats.dropped++;
			return;
		}
		memcpy(buf, p, n);
		/* Both connection events have the peer's address at the same place. */
		bool conn = n >= 14 && p[0] == BLE_HCI_EVCODE_LE_META &&
			    (p[2] == BLE_HCI_LE_SUBEV_CONN_COMPLETE || p[2] == BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE);
		if (conn && p[7] == BLE_ADDR_RANDOM) {
			uint8_t *b = buf;
			if (bt_store_resolve(p + 8, &b[7], b + 8))
				log_printf("bt: resolved private address of a bonded peer\n");
		}
		stats.events++;
		count('e', p[0], p[0] == BLE_HCI_EVCODE_LE_META && n > 2 ? p[2] : 0);
		ble_transport_to_hs_evt(buf);
	} else if (type == HCI_ACL) {
		struct os_mbuf *om = ble_transport_alloc_acl_from_ll();
		if (!om || os_mbuf_append(om, p, (uint16_t)n)) {
			if (om)
				os_mbuf_free_chain(om);
			stats.dropped++;
			return;
		}
		stats.acl_in++;
		bool start = n >= 9 && ((p[1] >> 4) & 3) != 1;
		if (start && (p[6] | p[7] << 8) == 4)
			count('a', p[8], n >= 11 ? (uint16_t)(p[9] | p[10] << 8) : 0);
		else
			count(start ? 'l' : 'f', start ? p[6] : 0, 0);
		ble_transport_to_hs_acl(om);
	} else {
		stats.dropped++;
	}
}

void bt_transport_attach(void)
{
	hci_set_handler(rx);
	bt_npl_set_wait(hci_poll);
	attached = true;
}

void bt_transport_detach(void)
{
	attached = false;
	bt_npl_set_wait(NULL);
	hci_set_handler(NULL);
}

void bt_transport_get_stats(struct bt_transport_stats *s)
{
	*s = stats;
}

void ble_transport_ll_init(void)
{
}

int ble_transport_to_ll_cmd_impl(void *buf)
{
	const uint8_t *p = buf;
	int r = attached ? hci_send(HCI_COMMAND, p, 3u + p[2]) : -1;
	ble_transport_free(buf);
	if (r) {
		stats.tx_errors++;
		return BLE_ERR_HW_FAIL;
	}
	stats.commands++;
	return 0;
}

int ble_transport_to_ll_acl_impl(struct os_mbuf *om)
{
	unsigned n = OS_MBUF_PKTLEN(om);
	int r = -1;
	if (attached && n <= sizeof acl_buf && !os_mbuf_copydata(om, 0, (int)n, acl_buf))
		r = hci_send(HCI_ACL, acl_buf, n);
	os_mbuf_free_chain(om);
	if (r) {
		stats.tx_errors++;
		return BLE_ERR_HW_FAIL;
	}
	stats.acl_out++;
	return 0;
}

int ble_transport_to_ll_iso_impl(struct os_mbuf *om)
{
	os_mbuf_free_chain(om);
	return BLE_ERR_UNSUPPORTED;
}
