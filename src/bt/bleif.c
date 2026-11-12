/* The Apollo3 Blue's own BLE controller, behind its BLE interface. The
 * link to it is not driven yet, so Bluetooth reports the controller as
 * missing and the rest of the watch runs without it. */
#include "asteroid.h"
#include "bt.h"

int bt_start(const uint8_t *bdaddr, struct bt_info *info)
{
	info->failed = "no driver for the built-in controller yet";
	info->status = -1;
	return -1;
}

void bt_stop(void) {}
bool bt_present(void) { return true; }
bool bt_running(void) { return false; }
bool bt_host_wake(void) { return false; }
bool bt_link_byte(uint8_t c) { return false; }
void bt_tx_activity(void) {}
void bt_lpm_poll(void) {}
uint32_t bt_lpm_next_ms(void) { return UINT32_MAX; }
bool bt_can_sleep(void) { return true; }
void bt_sleep(bool asleep) {}
