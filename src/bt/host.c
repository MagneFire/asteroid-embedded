/* The NimBLE host on top of the patched controller: started from the
 * console after bt_start(), run from the main loop by bt_poll(). On sync
 * it advertises the way asteroid-btsyncd does through AsteroidOS's BlueZ
 * (flags, the hostname as complete local name, the AsteroidOS service
 * UUID); pairing is LE Secure Connections with MITM and bonding,
 * DisplayYesNo like the watch's BlueZ agent. Host work runs at full CPU
 * speed even while the watch sleeps on XO: the P-256 key generation and
 * ECDH of a pairing could otherwise outlast the watchdog. */
#include <string.h>
#include "asteroid.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "bt.h"

#define ADV_ITVL_MIN	BLE_GAP_ADV_ITVL_MS(100)
#define ADV_ITVL_MAX	BLE_GAP_ADV_ITVL_MS(150)
#define BATTERY_CHECK_MS 60000
#define SYNC_TIMEOUT_MS	10000

static bool initialised, host_on;
static enum bt_host_state state;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t mtu;
static uint8_t own_addr_type;
static uint32_t battery_t, start_t, resets;
static unsigned char crypto_heap[32768];

static void advertise(void);

/* Asked for once the link is encrypted. The phone picks a 30-50 ms
 * interval and no latency, so the controller listens every interval even
 * when nothing is sent; with latency 4 it may skip four of every five
 * events while it has nothing to send. What the phone sends then waits up
 * to 250 ms; what the watch sends goes at the next event. */
static struct bt_params params = { 24, 40, 4, 500 };

/* Asking again costs a wakeup, staying at latency 0 costs radio listens
 * for the rest of the connection, so the watch never stops asking. */
#define PARAMS_RETRY_MS	5000
#define PARAMS_RETRY_MAX_MS	640000
#define PARAMS_HELD_MS	60000
/* A phone can accept the request without ever applying it, and NimBLE
 * reports nothing then. */
#define PARAMS_PENDING_MS	40000
static uint32_t params_at;
static uint32_t params_wait = PARAMS_RETRY_MS;
static uint32_t params_held_at;
static bool params_held;

static void params_again(void)
{
	if (params.itvl_max)
		params_at = (time_ms() + params_wait) | 1;
}

static void params_restart(void)
{
	params_wait = PARAMS_RETRY_MS;
	params_held = false;
	params_at = 0;
}

static bool params_short(uint16_t c)
{
	struct ble_gap_conn_desc d;
	return !ble_gap_conn_find(c, &d) && d.conn_latency < params.latency;
}

static void request_params(uint16_t c)
{
	if (!params.itvl_max)
		return;
	struct ble_gap_upd_params p = { .itvl_min = params.itvl_min,
					.itvl_max = params.itvl_max,
					.latency = params.latency,
					.supervision_timeout = params.timeout };
	int rc = ble_gap_update_params(c, &p);
	if (rc) {
		log_printf("bt: connection update not asked: %d\n", rc);
		params_again();
	} else {
		uint32_t wait = params_wait > PARAMS_PENDING_MS ? params_wait : PARAMS_PENDING_MS;
		params_at = (time_ms() + wait) | 1;
	}
}

static int mtu_done(uint16_t c, const struct ble_gatt_error *error, uint16_t m, void *arg)
{
	(void)c;
	(void)arg;
	(void)m;
	if (error->status)
		log_printf("bt: mtu exchange failed: %u\n", error->status);
	return 0;
}

void bt_host_params(const struct bt_params *p)
{
	params = *p;
	params_restart();
	if (host_on && conn != BLE_HS_CONN_HANDLE_NONE)
		request_params(conn);
}

void bt_host_get_params(struct bt_params *p)
{
	*p = params;
}

static void log_addr(const char *what, const ble_addr_t *a)
{
	log_printf("bt: %s %02x:%02x:%02x:%02x:%02x:%02x (type %u)\n", what, a->val[5], a->val[4], a->val[3], a->val[2],
		   a->val[1], a->val[0], a->type);
}

/* The number shown while pairing, and whether it waits for the user to
 * confirm that the phone shows the same (numeric comparison) rather than
 * for the phone to enter it. */
static int32_t passkey = -1;
static bool comparing;
static uint16_t pairing_conn;

int32_t bt_passkey(void)
{
	return passkey;
}

/* The user's answer to the pairing screen, which it then hides: a
 * comparison is answered, and refusing a passkey drops the connection. */
void bt_pairing_reply(bool accept)
{
	if (passkey < 0)
		return;
	if (comparing) {
		struct ble_sm_io io = { .action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = accept };
		int rc = ble_sm_inject_io(pairing_conn, &io);
		log_printf("bt: pairing, compare %s%s\n", accept ? "accepted" : "rejected", rc ? " (failed)" : "");
		comparing = false;
	} else if (!accept) {
		ble_gap_terminate(pairing_conn, BLE_ERR_REM_USER_CONN_TERM);
		log_printf("bt: pairing cancelled\n");
	}
	passkey = -1;
}

static void passkey_action(uint16_t handle, const struct ble_gap_passkey_params *p)
{
	struct ble_sm_io io = { .action = p->action };
	pairing_conn = handle;
	if (p->action == BLE_SM_IOACT_NUMCMP) {
		passkey = (int32_t)p->numcmp;
		comparing = true;
		log_printf("bt: pairing, compare %06lu\n", p->numcmp);
		return;
	} else if (p->action == BLE_SM_IOACT_DISP) {
		uint32_t r = 0;
		ble_hs_hci_rand(&r, sizeof r);
		io.passkey = r % 1000000;
		passkey = (int32_t)io.passkey;
		log_printf("bt: pairing, passkey %06lu\n", io.passkey);
	} else {
		log_printf("bt: pairing, unsupported action %u\n", p->action);
		return;
	}
	int rc = ble_sm_inject_io(handle, &io);
	if (rc)
		log_printf("bt: pairing input failed: %d\n", rc);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
	struct ble_gap_conn_desc d;
	switch (ev->type) {
	case BLE_GAP_EVENT_CONNECT:
		if (ev->connect.status) {
			log_printf("bt: connection failed: %d\n", ev->connect.status);
			advertise();
			break;
		}
		conn = ev->connect.conn_handle;
		mtu = BLE_ATT_MTU_DFLT;
		events_note("bt connected");
		state = BT_HOST_CONNECTED;
		if (!ble_gap_conn_find(conn, &d)) {
			struct ble_store_key_sec k = { .peer_addr = d.peer_id_addr };
			struct ble_store_value_sec v;
			log_addr("connected to", &d.peer_ota_addr);
			log_addr(ble_store_read_peer_sec(&k, &v) ? "no keys for" : "keys kept for", &d.peer_id_addr);
			log_printf("bt: interval %u x 1.25 ms, latency %u, timeout %u x 10 ms\n", d.conn_itvl, d.conn_latency,
				   d.supervision_timeout);
		}
		/* The phone may not ask for more than the default 23 bytes, which
		 * splits every longer write and notification into many packets. */
		if (ble_gattc_exchange_mtu(conn, mtu_done, NULL))
			log_printf("bt: mtu exchange not started\n");
		break;
	case BLE_GAP_EVENT_DISCONNECT:
		passkey = -1;
		comparing = false;
		log_printf("bt: disconnected, reason %04x\n", ev->disconnect.reason);
		{
			char what[32];
			snprintf(what, sizeof what, "bt disconnected %04x", ev->disconnect.reason);
			events_note(what);
		}
		conn = BLE_HS_CONN_HANDLE_NONE;
		params_at = 0;
		advertise();
		break;
	case BLE_GAP_EVENT_CONN_UPDATE:
		if (!ble_gap_conn_find(ev->conn_update.conn_handle, &d)) {
			log_printf("bt: connection update %d, interval %u, latency %u, timeout %u\n", ev->conn_update.status,
				   d.conn_itvl, d.conn_latency, d.supervision_timeout);
			if (d.conn_latency >= params.latency) {
				if (!params_held) {
					params_held = true;
					params_held_at = time_ms();
				}
				params_at = 0;
			} else {
				/* Going back after the latency held a while is the
				 * phone's own update, not a fight over parameters. */
				bool held = params_held && time_ms() - params_held_at >= PARAMS_HELD_MS;
				params_held = false;
				if (d.sec_state.encrypted) {
					if (held)
						params_wait = PARAMS_RETRY_MS;
					params_again();
				}
			}
		}
		break;
	case BLE_GAP_EVENT_ADV_COMPLETE:
		if (conn == BLE_HS_CONN_HANDLE_NONE)
			advertise();
		break;
	case BLE_GAP_EVENT_ENC_CHANGE:
		passkey = -1;
		comparing = false;
		if (!ble_gap_conn_find(ev->enc_change.conn_handle, &d)) {
			log_printf("bt: encryption %d: encrypted %u, authenticated %u, bonded %u, key size %u\n",
				   ev->enc_change.status, d.sec_state.encrypted, d.sec_state.authenticated,
				   d.sec_state.bonded, d.sec_state.key_size);
			log_addr("peer identity", &d.peer_id_addr);
		}
		if (!ev->enc_change.status) {
			params_restart();
			request_params(ev->enc_change.conn_handle);
		}
		break;
	case BLE_GAP_EVENT_PASSKEY_ACTION:
		passkey_action(ev->passkey.conn_handle, &ev->passkey.params);
		break;
	case BLE_GAP_EVENT_REPEAT_PAIRING:
		if (!ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d)) {
			log_addr("pairing again, forgetting", &d.peer_id_addr);
			ble_store_util_delete_peer(&d.peer_id_addr);
		}
		return BLE_GAP_REPEAT_PAIRING_RETRY;
	case BLE_GAP_EVENT_SUBSCRIBE:
		log_printf("bt: subscribe handle %u, notify %u, indicate %u\n", ev->subscribe.attr_handle,
			   ev->subscribe.cur_notify, ev->subscribe.cur_indicate);
		bt_gatt_subscribe(ev->subscribe.conn_handle, ev->subscribe.attr_handle, ev->subscribe.cur_notify);
		break;
	case BLE_GAP_EVENT_MTU:
		mtu = ev->mtu.value;
		log_printf("bt: mtu %u\n", mtu);
		break;
	default:
		break;
	}
	return 0;
}

static void advertise(void)
{
	static const uint8_t *name;
	struct ble_hs_adv_fields f;
	struct ble_gap_adv_params p;
	name = (const uint8_t *)ble_svc_gap_device_name();
	memset(&f, 0, sizeof f);
	f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
	f.name = name;
	f.name_len = (uint8_t)strlen((const char *)name);
	f.name_is_complete = 1;
	f.uuids128 = &bt_asteroid_adv_uuid;
	f.num_uuids128 = 1;
	f.uuids128_is_complete = 1;
	int rc = ble_gap_adv_set_fields(&f);
	if (rc) {
		log_printf("bt: advertising data failed: %d\n", rc);
		return;
	}
	memset(&p, 0, sizeof p);
	p.conn_mode = BLE_GAP_CONN_MODE_UND;
	p.disc_mode = BLE_GAP_DISC_MODE_GEN;
	p.itvl_min = ADV_ITVL_MIN;
	p.itvl_max = ADV_ITVL_MAX;
	rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
	if (rc) {
		log_printf("bt: advertising failed: %d\n", rc);
		return;
	}
	if (state != BT_HOST_ADVERTISING)
		events_note("bt advertising");
	state = BT_HOST_ADVERTISING;
	log_printf("bt: advertising as \"%s\"\n", name);
}

static void on_sync(void)
{
	ble_addr_t a;
	int rc = ble_hs_id_infer_auto(0, &own_addr_type);
	if (rc) {
		log_printf("bt: no usable address: %d\n", rc);
		return;
	}
	a.type = own_addr_type;
	ble_hs_id_copy_addr(own_addr_type, a.val, NULL);
	log_addr("host synced, address", &a);
	conn = BLE_HS_CONN_HANDLE_NONE;
	advertise();
}

static void on_reset(int reason)
{
	resets++;
	if (state != BT_HOST_STARTING)
		start_t = time_ms();
	state = BT_HOST_STARTING;
	conn = BLE_HS_CONN_HANDLE_NONE;
	log_printf("bt: host reset, reason %d\n", reason);
}

static int init(void)
{
	mbedtls_memory_buffer_alloc_init(crypto_heap, sizeof crypto_heap);
	nimble_port_init();
	ble_hs_cfg.sync_cb = on_sync;
	ble_hs_cfg.reset_cb = on_reset;
	ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_YESNO;
	ble_hs_cfg.sm_bonding = 1;
	ble_hs_cfg.sm_mitm = 1;
	ble_hs_cfg.sm_sc = 1;
	ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
	ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
	bt_store_init();
	ble_svc_gap_init();
	ble_svc_gap_device_name_set(board->name);
	ble_svc_gatt_init();
	int rc = bt_gatt_init();
	if (rc)
		log_printf("bt: gatt services failed: %d\n", rc);
	return rc;
}

int bt_host_start(void)
{
	bt_transport_attach();
	if (!initialised) {
		initialised = true;
		int rc = init();
		if (rc) {
			bt_transport_detach();
			return rc;
		}
	} else {
		ble_hs_sched_reset(BLE_HS_ECONTROLLER);
	}
	host_on = true;
	state = BT_HOST_STARTING;
	conn = BLE_HS_CONN_HANDLE_NONE;
	battery_t = start_t = time_ms();
	return 0;
}

void bt_host_stop(void)
{
	host_on = false;
	state = BT_HOST_OFF;
	conn = BLE_HS_CONN_HANDLE_NONE;
	params_restart();
	bt_transport_detach();
}

void bt_poll(void)
{
	if (!host_on)
		return;
	hci_poll();
	bt_lpm_poll();
	bool boost = cpu_clock_is_slow() && bt_idle_ms() == 0;
	if (boost)
		cpu_clock_slow(false);
	bt_npl_run(nimble_port_get_dflt_eventq(), 32);
	bt_gatt_poll();
	if (boost)
		cpu_clock_slow(true);
	if (state == BT_HOST_STARTING && time_ms() - start_t >= SYNC_TIMEOUT_MS) {
		log_printf("bt: host did not sync in %u ms, stopping it\n", SYNC_TIMEOUT_MS);
		bt_host_stop();
		state = BT_HOST_FAILED;
		return;
	}
	if (params_at && conn != BLE_HS_CONN_HANDLE_NONE && (int32_t)(time_ms() - params_at) >= 0) {
		params_at = 0;
		if (params_short(conn)) {
			if (params_wait < PARAMS_RETRY_MAX_MS)
				params_wait *= 2;
			request_params(conn);
		}
	}
	if (conn != BLE_HS_CONN_HANDLE_NONE && time_ms() - battery_t >= BATTERY_CHECK_MS) {
		battery_t = time_ms();
		bt_gatt_battery_check();
	}
}

uint32_t bt_idle_ms(void)
{
	if (!host_on)
		return UINT32_MAX;
	if (!ble_npl_eventq_is_empty(nimble_port_get_dflt_eventq()))
		return 0;
	uint32_t next = bt_npl_next_ms();
	if (params_at && conn != BLE_HS_CONN_HANDLE_NONE) {
		int32_t left = (int32_t)(params_at - time_ms());
		uint32_t at = left > 0 ? (uint32_t)left : 0;
		if (at < next)
			next = at;
	}
	return next;
}

bool bt_host_running(void)
{
	return host_on;
}

bool bt_host_connected(void)
{
	return host_on && conn != BLE_HS_CONN_HANDLE_NONE;
}

void bt_host_disconnect(void)
{
	if (bt_host_connected())
		ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

void bt_host_status(struct bt_status *s)
{
	struct ble_gap_conn_desc d;
	ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
	int n = 0;
	memset(s, 0, sizeof *s);
	s->state = state;
	s->synced = host_on && ble_hs_synced();
	s->resets = resets;
	s->next_ms = bt_idle_ms();
	s->kv = bt_kv_name();
	bt_transport_get_stats(&s->transport);
	if (host_on && conn != BLE_HS_CONN_HANDLE_NONE && !ble_gap_conn_find(conn, &d)) {
		s->connected = true;
		memcpy(s->peer, d.peer_id_addr.val, 6);
		s->peer_type = d.peer_id_addr.type;
		s->encrypted = d.sec_state.encrypted;
		s->authenticated = d.sec_state.authenticated;
		s->bonded = d.sec_state.bonded;
		s->interval = d.conn_itvl;
		s->latency = d.conn_latency;
		s->timeout = d.supervision_timeout;
		s->mtu = mtu;
	}
	if (initialised && !ble_store_util_bonded_peers(peers, &n, (int)ARRAY_SIZE(peers))) {
		s->bonds = (unsigned)n;
		for (int i = 0; i < n && i < (int)ARRAY_SIZE(s->bond_addr); i++) {
			memcpy(s->bond_addr[i], peers[i].val, 6);
			s->bond_type[i] = peers[i].type;
		}
	}
}
