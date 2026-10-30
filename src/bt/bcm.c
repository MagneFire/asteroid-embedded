/* Broadcom BCM43430A0 Bluetooth on BLSP1 UART4 (gpio 12-15, function 2).
 * Power: REG_ON on gpio 67 (low 30 ms, then high). Sleep handshake: the
 * host drives BT_WAKE on gpio 63, the chip drives HOST_WAKE on gpio 64,
 * both active low. Bring-up as brcm_patchram_plus runs it on this watch
 * (--use_baudrate_for_download --no2bytes --enable_lpm, 3000000 baud):
 * reset at 115200, switch to the fast rate, download the minidriver,
 * send the .hcd file's records (each a vendor command, the last one
 * launches the patch), back to 115200 while the patched firmware boots,
 * reset, fast rate again, optional BD_ADDR, sleep mode parameters. */
#include <string.h>
#include "asteroid.h"
#include "bt.h"

#define GPIO_BT_UART		12
#define FUNC_BT_UART		2
#define GPIO_BT_DEV_WAKE	63
#define GPIO_BT_HOST_WAKE	64
#define GPIO_BT_REG_ON		67
#define MPM_PIN_HOST_WAKE	28
#define DEV_WAKE_IDLE_MS	1000
#define BAUD_BOOT		115200
#define BAUD_FAST		3000000
#define PATCH_NAME		"4343A0.hcd"

#define HCI_RESET		0x0c03
#define HCI_READ_LOCAL_NAME	0x0c14
#define HCI_READ_LOCAL_VERSION	0x1001
#define HCI_READ_BD_ADDR	0x1009
#define BCM_WRITE_BD_ADDR	0xfc01
#define BCM_UPDATE_BAUD_RATE	0xfc18
#define BCM_WRITE_SLEEP_MODE	0xfc27
#define BCM_DOWNLOAD_MINIDRIVER	0xfc2e
#define BCM_LAUNCH_RAM		0xfc4e

static const uint8_t sleep_mode[12] = { 1, 0x18, 0x18, 0, 0, 1, 1, 0, 0, 0, 0, 0 };

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
	tlmm_set(GPIO_BT_REG_ON, 0);
	tlmm_config(GPIO_BT_REG_ON, 0, TLMM_OUTPUT, TLMM_PULL_DOWN, 2);
	if (!on)
		return;
	tlmm_set(GPIO_BT_DEV_WAKE, 0);
	tlmm_config(GPIO_BT_DEV_WAKE, 0, TLMM_OUTPUT, TLMM_PULL_DOWN, 2);
	tlmm_config(GPIO_BT_HOST_WAKE, 0, TLMM_INPUT, TLMM_PULL_DOWN, 2);
	sleep_ms(30);
	tlmm_set(GPIO_BT_REG_ON, 1);
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

static int set_baud(uint32_t baud)
{
	uint8_t p[6] = { 0, 0, (uint8_t)baud, (uint8_t)(baud >> 8), (uint8_t)(baud >> 16), (uint8_t)(baud >> 24) };
	int r = hci_cmd(BCM_UPDATE_BAUD_RATE, p, sizeof p, NULL, NULL, 1000);
	if (r)
		return r;
	return uart_dm_set_baud(BT_UART, baud);
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
	uart_dm_stats(BT_UART, &rx, &tx, &uerr);
	hci_stats(&pkts, &herr);
	info->failed = what;
	info->status = r;
	log_printf("bt: %s failed: %d, cts %u, host wake %u, uart %lu/%lu/%lu, hci %lu/%lu\n", what, r, uart_dm_cts(BT_UART),
		   tlmm_get(GPIO_BT_HOST_WAKE), rx, tx, uerr, pkts, herr);
	return r ? r : -1;
}

static void host_wake_irq(unsigned n)
{
	tlmm_irq_ack(n);
	bt_tx_activity();
}

int bt_start(const uint8_t *bdaddr, struct bt_info *info)
{
	uint32_t hcd_len;
	const uint8_t *hcd = boot_blob(PATCH_NAME, &hcd_len);
	memset(info, 0, sizeof *info);
	uint32_t t0 = time_ms();
	if (!hcd)
		return fail(info, "patch file " PATCH_NAME " in the boot image", -1);
	if (running)
		bt_stop();
	hci_reset_parser();
	hci_set_handler(log_packet);
	power(true);
	sleep_ms(100);
	if (uart_dm_init(BT_UART, GPIO_BT_UART, FUNC_BT_UART, BAUD_BOOT, true))
		return fail(info, "uart", -1);
	running = true;
	log_printf("bt: powered, cts %u, host wake %u\n", uart_dm_cts(BT_UART), tlmm_get(GPIO_BT_HOST_WAKE));
	int r;
	if ((r = reset()))
		return fail(info, "reset", r);
	if ((r = set_baud(BAUD_FAST)))
		return fail(info, "baud rate for the download", r);
	uint32_t t1 = time_ms();
	if ((r = patchram(hcd, hcd_len)))
		return fail(info, "patch download", r);
	info->patch_ms = time_ms() - t1;
	uart_dm_set_baud(BT_UART, BAUD_BOOT);
	sleep_ms(250);
	uint8_t junk[64];
	while (uart_dm_read(BT_UART, junk, sizeof junk))
		;
	hci_reset_parser();
	if ((r = reset()))
		return fail(info, "reset after the patch", r);
	if ((r = set_baud(BAUD_FAST)))
		return fail(info, "baud rate", r);
	if (bdaddr) {
		uint8_t a[6];
		for (int i = 0; i < 6; i++)
			a[i] = bdaddr[5 - i];
		if ((r = hci_cmd(BCM_WRITE_BD_ADDR, a, 6, NULL, NULL, 1000)))
			return fail(info, "write bd_addr", r);
	}
	if ((r = hci_cmd(BCM_WRITE_SLEEP_MODE, sleep_mode, sizeof sleep_mode, NULL, NULL, 1000)))
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
	uart_dm_stats(BT_UART, &rx, &tx, &uerr);
	hci_stats(&pkts, &herr);
	log_printf("bt: up in %lu ms (patch %lu ms), lmp subversion %04x, host wake %u, uart %lu/%lu/%lu, hci %lu/%lu\n",
		   info->total_ms, info->patch_ms, info->lmp_subversion, tlmm_get(GPIO_BT_HOST_WAKE), rx, tx, uerr, pkts,
		   herr);
	dev_awake = true;
	tx_t = time_ms();
	tlmm_irq_config(GPIO_BT_HOST_WAKE, TLMM_IRQ_EDGE_FALLING, host_wake_irq);
	tlmm_irq_enable(GPIO_BT_HOST_WAKE, true);
	spm_wake_pin(MPM_PIN_HOST_WAKE, true, false);
	return 0;
}

void bt_stop(void)
{
	spm_wake_pin(MPM_PIN_HOST_WAKE, false, false);
	tlmm_irq_enable(GPIO_BT_HOST_WAKE, false);
	uart_dm_stop(BT_UART);
	power(false);
	running = false;
	dev_awake = true;
}

bool bt_running(void)
{
	return running;
}

bool bt_host_wake(void)
{
	return tlmm_get(GPIO_BT_HOST_WAKE);
}


void bt_tx_activity(void)
{
	tx_t = time_ms();
	if (!dev_awake) {
		tlmm_set(GPIO_BT_DEV_WAKE, 0);
		dev_awake = true;
	}
}

void bt_lpm_poll(void)
{
	if (running && !bt_host_wake())
		bt_tx_activity();
	if (running && dev_awake && time_ms() - tx_t >= DEV_WAKE_IDLE_MS && uart_dm_idle(BT_UART)) {
		tlmm_set(GPIO_BT_DEV_WAKE, 1);
		dev_awake = false;
	}
}

bool bt_can_sleep(void)
{
	return !running || (!dev_awake && bt_host_wake() && uart_dm_idle(BT_UART));
}

void bt_sleep(bool asleep)
{
	if (running)
		uart_dm_hold_rx(BT_UART, asleep);
}
