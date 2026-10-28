#ifndef BT_H
#define BT_H
#include <stdint.h>
#include <stdbool.h>

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
#endif
