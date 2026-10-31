#ifndef BT_H
#define BT_H
#include <stdint.h>
#include <stdbool.h>

#define BT_UART		4

#define HCI_COMMAND	1
#define HCI_ACL		2
#define HCI_SCO		3
#define HCI_EVENT	4

#define HCI_EV_COMMAND_COMPLETE	0x0e
#define HCI_EV_COMMAND_STATUS	0x0f

typedef void (*hci_packet_fn)(uint8_t type, const uint8_t *p, unsigned n);

void hci_reset_parser(void);
void hci_set_handler(hci_packet_fn fn);
void hci_poll(void);
int hci_send(uint8_t type, const uint8_t *p, unsigned n);
int hci_cmd(uint16_t opcode, const void *param, uint8_t plen, uint8_t *rsp, unsigned *rsp_len, uint32_t timeout_ms);
void hci_stats(uint32_t *packets, uint32_t *errors);

struct bt_info {
	uint8_t hci_version, lmp_version;
	uint16_t hci_revision, manufacturer, lmp_subversion;
	uint8_t bdaddr[6];
	char name[32];
	uint32_t patch_ms, total_ms;
	const char *failed;
	int status;
};

int bt_start(const uint8_t *bdaddr, struct bt_info *info);
void bt_stop(void);
bool bt_running(void);
bool bt_host_wake(void);
void bt_tx_activity(void);
void bt_lpm_poll(void);
uint32_t bt_lpm_next_ms(void);
bool bt_can_sleep(void);
void bt_sleep(bool asleep);

enum bt_host_state { BT_HOST_OFF, BT_HOST_STARTING, BT_HOST_ADVERTISING, BT_HOST_CONNECTED, BT_HOST_FAILED };

struct bt_transport_stats {
	uint32_t commands, events, acl_out, acl_in, dropped, tx_errors;
};

struct bt_status {
	enum bt_host_state state;
	bool synced, connected, encrypted, authenticated, bonded;
	uint8_t peer[6], peer_type;
	uint16_t interval, mtu;
	unsigned bonds;
	uint8_t bond_addr[4][6], bond_type[4];
	uint32_t resets, next_ms;
	const char *kv;
	struct bt_transport_stats transport;
};

int bt_host_start(void);
void bt_host_stop(void);
bool bt_host_running(void);
void bt_host_status(struct bt_status *s);
bool bt_host_connected(void);
void bt_host_disconnect(void);
void bt_poll(void);
int32_t bt_passkey(void);
void bt_pairing_reply(bool accept);
uint32_t bt_idle_ms(void);

struct bt_kv {
	const char *name;
	int (*load)(const char *key, void *buf, unsigned len);
	int (*save)(const char *key, const void *buf, unsigned len);
};

const char *bt_kv_name(void);
int bt_addr_load(uint8_t addr[6]);
int bt_addr_save(const uint8_t addr[6]);

struct ble_npl_eventq;
void bt_npl_set_wait(void (*fn)(void));
void bt_npl_expire(void);
unsigned bt_npl_run(struct ble_npl_eventq *evq, unsigned max);
uint32_t bt_npl_next_ms(void);
void bt_transport_attach(void);
void bt_transport_detach(void);
void bt_transport_get_stats(struct bt_transport_stats *s);
void bt_store_init(void);
int bt_store_clear(void);
bool bt_store_bonded(void);
bool bt_store_resolve(const uint8_t rpa[6], uint8_t *type, uint8_t addr[6]);
int bt_gatt_init(void);
void bt_gatt_battery_check(void);
void bt_gatt_subscribe(uint16_t conn, uint16_t attr, bool on);
void bt_gatt_poll(void);
int bt_gatt_media_command(const uint8_t *v, unsigned n);
bool bt_gatt_screenshot_requested(void);
void bt_gatt_screenshot_send(const uint8_t *p, uint32_t n);
bool bt_gatt_screenshot_busy(void);
#ifdef H_BLE_UUID_
extern const ble_uuid128_t bt_asteroid_adv_uuid;
#endif
#endif
