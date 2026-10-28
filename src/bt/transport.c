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
		if (n >= 14 && p[0] == BLE_HCI_EVCODE_LE_META && p[2] == BLE_HCI_LE_SUBEV_CONN_COMPLETE && p[7] == BLE_ADDR_RANDOM) {
			uint8_t *b = buf;
			if (bt_store_resolve(p + 8, &b[7], b + 8))
				log_printf("bt: resolved private address of a bonded peer\n");
		}
		stats.events++;
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
