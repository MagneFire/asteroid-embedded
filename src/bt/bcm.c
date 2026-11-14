/* Broadcom BCM4343 Bluetooth on a BLSP UART (function and gpio, REG_ON,
 * the wake lines, their polarity and the MPM pin come from board.h).
 * Power: REG_ON low 30 ms, then high. Sleep handshake: the host drives
 * BT_WAKE, the chip drives HOST_WAKE, each asserted at the board's active
 * level. Bring-up: reset at 115200, switch to the fast rate, download the
 * minidriver, send the .hcd file's records (each a vendor command, the
 * last one launches the patch), back to 115200 while the patched firmware
 * boots, reset, fast rate again, optional BD_ADDR, the board's sleep mode
 * parameters. */
#include <string.h>
#include "asteroid.h"
#include "bt.h"

#define GPIO_BT_UART		(board->bt.gpio)
#define FUNC_BT_UART		(board->bt.func)
#define GPIO_BT_DEV_WAKE	(board->bt.dev_wake)
#define GPIO_BT_HOST_WAKE	(board->bt.host_wake)
#define GPIO_BT_REG_ON		(board->bt.reg_on)
#define MPM_PIN_HOST_WAKE	(board->bt.mpm)
#define DEV_WAKE_ACTIVE		(board->bt.dev_wake_active)
#define HOST_WAKE_ACTIVE	(board->bt.host_wake_active)
#define DEV_WAKE_IDLE_MS	1000
#define BAUD_BOOT		115200
#define BAUD_FAST		3000000
#define PATCH_NAME		(board->bt.patch)

#define HCI_RESET		0x0c03
#define HCI_READ_LOCAL_NAME	0x0c14
#define HCI_READ_LOCAL_VERSION	0x1001
#define HCI_READ_BD_ADDR	0x1009
#define BCM_WRITE_BD_ADDR	0xfc01
#define BCM_UPDATE_BAUD_RATE	0xfc18
#define BCM_WRITE_SLEEP_MODE	0xfc27
#define BCM_DOWNLOAD_MINIDRIVER	0xfc2e
#define BCM_LAUNCH_RAM		0xfc4e


static bool dev_awake = true;
static uint32_t tx_t;
static bool running;

static void sleep_ms(uint32_t ms)
{
	for (uint32_t t0 = time_ms(); time_ms() - t0 < ms;)
		wdt_feed();
}

static void power(bool on)
{
	gpio_set(GPIO_BT_REG_ON, 0);
	gpio_config(GPIO_BT_REG_ON, 0, GPIO_OUTPUT, GPIO_PULL_DOWN, 2);
	if (board->bt.ldo) {
		int r = on ? regulator_on(board->bt.ldo, board->bt.ldo_uv) : regulator_off(board->bt.ldo);
		if (r)
			log_printf("bt: l%u %s not acknowledged\n", board->bt.ldo, on ? "on" : "off");
	}
	if (!on)
		return;
	gpio_set(GPIO_BT_DEV_WAKE, DEV_WAKE_ACTIVE);
	gpio_config(GPIO_BT_DEV_WAKE, 0, GPIO_OUTPUT, GPIO_PULL_DOWN, 2);
	gpio_config(GPIO_BT_HOST_WAKE, 0, GPIO_INPUT, GPIO_PULL_DOWN, 2);
	sleep_ms(30);
	gpio_set(GPIO_BT_REG_ON, 1);
}

static void log_packet(uint8_t type, const uint8_t *p, unsigned n)
{
	if (type == HCI_EVENT)
		log_printf("bt: event %02x, %u bytes\n", p[0], n);
	else
		log_printf("bt: packet type %u, %u bytes\n", type, n);
}

static int reset(void)
{
	int r = -1;
	for (int i = 0; i < 3 && r; i++)
		r = hci_cmd(HCI_RESET, NULL, 0, NULL, NULL, 1000);
	return r;
}

/* The rate asked of the controller and the one the host's UART then uses. */
static int set_baud(uint32_t baud, uint32_t host)
{
	uint8_t p[6] = { 0, 0, (uint8_t)baud, (uint8_t)(baud >> 8), (uint8_t)(baud >> 16), (uint8_t)(baud >> 24) };
	int r = hci_cmd(BCM_UPDATE_BAUD_RATE, p, sizeof p, NULL, NULL, 1000);
	if (r)
		return r;
	return uart_set_baud(BT_UART, host);
}

static int patchram(const uint8_t *hcd, uint32_t len)
{
	int r = hci_cmd(BCM_DOWNLOAD_MINIDRIVER, NULL, 0, NULL, NULL, 1000);
	if (r)
		return r;
	sleep_ms(50);
	uint32_t off = 0;
	unsigned records = 0;
	while (off + 3 <= len) {
		uint16_t op = (uint16_t)(hcd[off] | hcd[off + 1] << 8);
		uint8_t plen = hcd[off + 2];
		if (off + 3 + plen > len)
			return -3;
		r = hci_cmd(op, hcd + off + 3, plen, NULL, NULL, 1000);
		if (r) {
			log_printf("bt: patch record %u (%04x at %lu) failed: %d\n", records, op, off, r);
			return r;
		}
		records++;
		off += 3u + plen;
		if (op == BCM_LAUNCH_RAM)
			break;
	}
	log_printf("bt: %u patch records, %lu of %lu bytes\n", records, off, len);
	return 0;
}

static int fail(struct bt_info *info, const char *what, int r)
{
	uint32_t rx, tx, uerr, pkts, herr;
	uart_stats(BT_UART, &rx, &tx, &uerr);
	hci_stats(&pkts, &herr);
	info->failed = what;
	info->status = r;
	char bad[32];
	hci_bad_bytes(bad, sizeof bad);
	log_printf("bt: %s failed: %d, cts %u, host wake %u, uart %lu/%lu/%lu, hci %lu/%lu%s%s\n", what, r, uart_cts(BT_UART),
		   gpio_get(GPIO_BT_HOST_WAKE), rx, tx, uerr, pkts, herr, bad[0] ? ", stray" : "", bad);
	return r ? r : -1;
}

static void host_wake_irq(unsigned n)
{
	gpio_irq_ack(n);
	bt_tx_activity();
}

int bt_start(const uint8_t *bdaddr, struct bt_info *info)
{
	uint32_t hcd_len;
	const uint8_t *hcd = firmware_get(PATCH_NAME, &hcd_len);
	memset(info, 0, sizeof *info);
	uint32_t t0 = time_ms();
	if (!hcd)
		return fail(info, "patch file", -1);
	if (running)
		bt_stop();
	hci_reset_parser();
	hci_set_handler(log_packet);
	uint32_t rom_baud = board->bt.rom_baud ? board->bt.rom_baud : BAUD_BOOT;
	uint32_t rom_fast = board->bt.rom_fast ? board->bt.rom_fast : BAUD_FAST;
	if (board->bt.rom_baud) {
		/* TX idle and RFR deasserted while the chip starts: with RFR
		 * asserted it starts in autobaud mode, which takes no baud rate
		 * change before the patch. */
		if (uart_init(BT_UART, GPIO_BT_UART, FUNC_BT_UART, rom_baud, true))
			return fail(info, "uart", -1);
		uart_hold_rfr(BT_UART, true);
		power(true);
		sleep_ms(100);
		uart_hold_rfr(BT_UART, false);
		sleep_ms(10);
	} else {
		power(true);
		sleep_ms(100);
		if (uart_init(BT_UART, GPIO_BT_UART, FUNC_BT_UART, BAUD_BOOT, true))
			return fail(info, "uart", -1);
	}
	running = true;
	log_printf("bt: powered, cts %u, host wake %u\n", uart_cts(BT_UART), gpio_get(GPIO_BT_HOST_WAKE));
	int r;
	if ((r = reset()))
		return fail(info, "reset", r);
	if ((r = set_baud(BAUD_FAST, rom_fast)))
		return fail(info, "baud rate for the download", r);
	uint32_t t1 = time_ms();
	if ((r = patchram(hcd, hcd_len)))
		return fail(info, "patch download", r);
	info->patch_ms = time_ms() - t1;
	uart_set_baud(BT_UART, BAUD_BOOT);
	sleep_ms(250);
	uint8_t junk[64];
	while (uart_read(BT_UART, junk, sizeof junk))
		;
	hci_reset_parser();
	if ((r = reset()))
		return fail(info, "reset after the patch", r);
	if ((r = set_baud(BAUD_FAST, BAUD_FAST)))
		return fail(info, "baud rate", r);
	if (bdaddr) {
		uint8_t a[6];
		for (int i = 0; i < 6; i++)
			a[i] = bdaddr[5 - i];
		if ((r = hci_cmd(BCM_WRITE_BD_ADDR, a, 6, NULL, NULL, 1000)))
			return fail(info, "write bd_addr", r);
	}
	if ((r = hci_cmd(BCM_WRITE_SLEEP_MODE, board->bt.sleep_mode, sizeof board->bt.sleep_mode, NULL, NULL, 1000)))
		return fail(info, "sleep mode", r);
	uint8_t rsp[249];
	unsigned n = 9;
	if ((r = hci_cmd(HCI_READ_LOCAL_VERSION, NULL, 0, rsp, &n, 1000)) || n < 9)
		return fail(info, "read local version", r);
	info->hci_version = rsp[1];
	info->hci_revision = (uint16_t)(rsp[2] | rsp[3] << 8);
	info->lmp_version = rsp[4];
	info->manufacturer = (uint16_t)(rsp[5] | rsp[6] << 8);
	info->lmp_subversion = (uint16_t)(rsp[7] | rsp[8] << 8);
	n = 7;
	if ((r = hci_cmd(HCI_READ_BD_ADDR, NULL, 0, rsp, &n, 1000)) || n < 7)
		return fail(info, "read bd_addr", r);
	for (int i = 0; i < 6; i++)
		info->bdaddr[i] = rsp[6 - i];
	n = sizeof rsp;
	if (!hci_cmd(HCI_READ_LOCAL_NAME, NULL, 0, rsp, &n, 1000) && n > 1) {
		unsigned k = n - 1 < sizeof info->name - 1 ? n - 1 : sizeof info->name - 1;
		memcpy(info->name, rsp + 1, k);
		info->name[k] = 0;
	}
	info->total_ms = time_ms() - t0;
	uint32_t rx, tx, uerr, pkts, herr;
	uart_stats(BT_UART, &rx, &tx, &uerr);
	hci_stats(&pkts, &herr);
	log_printf("bt: up in %lu ms (patch %lu ms), lmp subversion %04x, host wake %u, uart %lu/%lu/%lu, hci %lu/%lu\n",
		   info->total_ms, info->patch_ms, info->lmp_subversion, gpio_get(GPIO_BT_HOST_WAKE), rx, tx, uerr, pkts,
		   herr);
	dev_awake = true;
	tx_t = time_ms();
	gpio_irq_config(GPIO_BT_HOST_WAKE, HOST_WAKE_ACTIVE ? GPIO_IRQ_EDGE_RISING : GPIO_IRQ_EDGE_FALLING, host_wake_irq);
	gpio_irq_enable(GPIO_BT_HOST_WAKE, true);
	wake_pin(MPM_PIN_HOST_WAKE, true, HOST_WAKE_ACTIVE);
	return 0;
}

void bt_stop(void)
{
	wake_pin(MPM_PIN_HOST_WAKE, false, HOST_WAKE_ACTIVE);
	gpio_irq_enable(GPIO_BT_HOST_WAKE, false);
	uart_stop(BT_UART);
	power(false);
	running = false;
	dev_awake = true;
}

bool bt_running(void)
{
	return running;
}

/* The raw HOST_WAKE level, whatever the board's polarity. */
bool bt_host_wake(void)
{
	return gpio_get(GPIO_BT_HOST_WAKE);
}

static bool host_wake_asserted(void)
{
	return bt_host_wake() == HOST_WAKE_ACTIVE;
}


bool bt_link_byte(uint8_t c) { return false; }

void bt_tx_activity(void)
{
	tx_t = time_ms();
	if (!dev_awake) {
		gpio_set(GPIO_BT_DEV_WAKE, DEV_WAKE_ACTIVE);
		dev_awake = true;
	}
}

void bt_lpm_poll(void)
{
	if (running && host_wake_asserted())
		bt_tx_activity();
	if (running && dev_awake && time_ms() - tx_t >= DEV_WAKE_IDLE_MS && uart_idle(BT_UART)) {
		gpio_set(GPIO_BT_DEV_WAKE, !DEV_WAKE_ACTIVE);
		dev_awake = false;
	}
}

uint32_t bt_lpm_next_ms(void)
{
	if (!running || !dev_awake)
		return UINT32_MAX;
	uint32_t idle = time_ms() - tx_t;
	return idle >= DEV_WAKE_IDLE_MS ? 1 : DEV_WAKE_IDLE_MS - idle;
}

bool bt_can_sleep(void)
{
	return !running || (!dev_awake && !host_wake_asserted() && uart_idle(BT_UART));
}

void bt_sleep(bool asleep)
{
	if (running)
		uart_hold_rx(BT_UART, asleep);
}
