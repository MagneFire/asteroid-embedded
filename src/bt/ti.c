/* TI WiLink Bluetooth on a UART with RTS and CTS. Power is one line,
 * low for 5 ms and then high. The chip starts at 115200 and is told
 * the fast rate with a vendor command, answered at the old rate. Its
 * firmware is a script from the system partition: a 32-byte header,
 * then records of a type and a length, of which the ones that matter
 * are whole command packets to send; each is answered with a command
 * complete, which is all the wait records in between ask for. The
 * script's last commands switch on the chip's sleep protocol, single
 * bytes between packets: the chip says when it goes to sleep and is
 * told that is fine, and whichever side has something to send says
 * wake up first and waits for the other's answer. Low energy is a
 * vendor command of its own after the script. */
#include <string.h>
#include "asteroid.h"
#include "bt.h"

#define GPIO_ENABLE		(board->bt.reg_on)
#define WAKE_PAD		(board->bt.mpm)
#define SCRIPT_NAME		(board->bt.patch)
#define BAUD_BOOT		115200
#define BAUD_FAST		3000000

#define HCI_READ_LOCAL_NAME	0x0c14
#define HCI_READ_LOCAL_VERSION	0x1001
#define HCI_READ_BD_ADDR	0x1009
#define TI_WRITE_BD_ADDR	0xfc06
#define TI_LE_ENABLE		0xfd5b
#define TI_UPDATE_BAUD_RATE	0xff36

#define SCRIPT_MAGIC		0x42535442u	/* "BTSB" */
#define SCRIPT_HEADER		32
#define ACTION_SEND		1
#define ACTION_DELAY		4

#define LL_SLEEP_IND		0x30
#define LL_SLEEP_ACK		0x31
#define LL_WAKE_IND		0x32
#define LL_WAKE_ACK		0x33
#define WAKE_WAIT_MS		50
#define WAKE_TRIES		5

static enum { AWAKE, ASLEEP } chip = AWAKE;
static bool running;
static uint32_t wake_failures;

static void sleep_ms(uint32_t ms)
{
	for (uint32_t t0 = time_ms(); time_ms() - t0 < ms;)
		wdt_feed();
}

static void link_send(uint8_t c)
{
	uart_write(BT_UART, &c, 1);
}

/* From the packet parser, so between packets. */
bool bt_link_byte(uint8_t c)
{
	if (c < LL_SLEEP_IND || c > LL_WAKE_ACK)
		return false;
	if (c == LL_SLEEP_IND) {
		link_send(LL_SLEEP_ACK);
		chip = ASLEEP;
	} else if (c == LL_WAKE_IND) {
		link_send(LL_WAKE_ACK);
		chip = AWAKE;
	} else if (c == LL_WAKE_ACK)
		chip = AWAKE;
	return true;
}

/* Before anything is sent: a sleeping chip is woken first. It was not
 * sending, so its answer is the next byte to come, or its own wake-up
 * if it had just decided to send something too. */
void bt_tx_activity(void)
{
	if (!running || chip == AWAKE)
		return;
	for (int attempt = 0; attempt < WAKE_TRIES && chip != AWAKE; attempt++) {
		link_send(LL_WAKE_IND);
		for (uint32_t t0 = time_ms(); chip != AWAKE && time_ms() - t0 < WAKE_WAIT_MS;) {
			uint8_t c;
			if (uart_read(BT_UART, &c, 1))
				hci_rx_byte(c);
		}
	}
	if (chip != AWAKE) {
		wake_failures++;
		chip = AWAKE;
	}
}

static void power(bool on)
{
	gpio_set(GPIO_ENABLE, 0);
	gpio_config(GPIO_ENABLE, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	if (!on)
		return;
	sleep_ms(5);
	gpio_set(GPIO_ENABLE, 1);
	sleep_ms(100);
}

static void log_packet(uint8_t type, const uint8_t *p, unsigned n)
{
	if (type == HCI_EVENT)
		log_printf("bt: event %02x, %u bytes\n", p[0], n);
	else
		log_printf("bt: packet type %u, %u bytes\n", type, n);
}

static int fail(struct bt_info *info, const char *what, int r)
{
	uint32_t rx, tx, uerr, pkts, herr;
	uart_stats(BT_UART, &rx, &tx, &uerr);
	hci_stats(&pkts, &herr);
	info->failed = what;
	info->status = r;
	log_printf("bt: %s failed: %d, cts %u, uart %lu/%lu/%lu, hci %lu/%lu\n", what, r, uart_cts(BT_UART), rx, tx, uerr, pkts,
		   herr);
	return r ? r : -1;
}

static int set_baud(uint32_t baud)
{
	uint8_t p[4] = { (uint8_t)baud, (uint8_t)(baud >> 8), (uint8_t)(baud >> 16), (uint8_t)(baud >> 24) };
	int r = hci_cmd(TI_UPDATE_BAUD_RATE, p, sizeof p, NULL, NULL, 1000);
	if (r)
		return r;
	return uart_set_baud(BT_UART, baud);
}

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

/* The script's commands, each sent once the one before is answered. A
 * change of baud rate in a script is skipped: the rate is set already. */
static int script_run(const uint8_t *s, uint32_t len)
{
	if (len < SCRIPT_HEADER || le32(s) != SCRIPT_MAGIC)
		return -3;
	unsigned sent = 0;
	for (uint32_t off = SCRIPT_HEADER; off + 4 <= len;) {
		uint32_t type = le16(s + off), size = le16(s + off + 2);
		const uint8_t *d = s + off + 4;
		if (size > len - off - 4)
			return -3;
		off += 4 + size;
		if (type == ACTION_DELAY && size >= 4)
			sleep_ms(le32(d));
		if (type != ACTION_SEND)
			continue;
		if (size < 4 || d[0] != HCI_COMMAND || 4u + d[3] != size)
			return -3;
		uint16_t op = (uint16_t)le16(d + 1);
		if (op == TI_UPDATE_BAUD_RATE)
			continue;
		int r = hci_cmd(op, d + 4, d[3], NULL, NULL, 1000);
		if (r) {
			log_printf("bt: script command %u (%04x) failed: %d\n", sent, op, r);
			return r;
		}
		sent++;
	}
	log_printf("bt: %u script commands, %lu bytes\n", sent, len);
	return 0;
}

static int read_bdaddr(uint8_t a[6])
{
	uint8_t rsp[7];
	unsigned n = sizeof rsp;
	int r = hci_cmd(HCI_READ_BD_ADDR, NULL, 0, rsp, &n, 1000);
	if (r || n < 7)
		return r ? r : -1;
	for (int i = 0; i < 6; i++)
		a[i] = rsp[6 - i];
	return 0;
}

/* The address as the chip wants it written is not known for sure, so
 * what it says it has afterwards decides: if that is the address
 * backwards, it is written the other way round. */
static int write_bdaddr(const uint8_t *bdaddr)
{
	uint8_t a[6], got[6];
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < 6; i++)
			a[i] = pass ? bdaddr[5 - i] : bdaddr[i];
		int r = hci_cmd(TI_WRITE_BD_ADDR, a, 6, NULL, NULL, 1000);
		if (r || (r = read_bdaddr(got)))
			return r;
		if (!memcmp(got, bdaddr, 6))
			return 0;
	}
	return -4;
}

int bt_start(const uint8_t *bdaddr, struct bt_info *info)
{
	uint32_t script_len;
	const uint8_t *script = firmware_get(SCRIPT_NAME, &script_len);
	memset(info, 0, sizeof *info);
	uint32_t t0 = time_ms();
	if (!script)
		return fail(info, "script file", -1);
	if (running)
		bt_stop();
	hci_reset_parser();
	hci_set_handler(log_packet);
	chip = AWAKE;
	if (uart_init(BT_UART, 0, 0, BAUD_BOOT, true))
		return fail(info, "uart", -1);
	power(true);
	running = true;
	log_printf("bt: powered, cts %u\n", uart_cts(BT_UART));
	int r;
	uint8_t rsp[249];
	unsigned n = 9;
	if ((r = hci_cmd(HCI_READ_LOCAL_VERSION, NULL, 0, rsp, &n, 1000)) || n < 9)
		return fail(info, "read local version", r);
	info->hci_version = rsp[1];
	info->hci_revision = (uint16_t)(rsp[2] | rsp[3] << 8);
	info->lmp_version = rsp[4];
	info->manufacturer = (uint16_t)(rsp[5] | rsp[6] << 8);
	/* The script's name is made of this: chip, major and minor. */
	uint16_t sub = (uint16_t)(rsp[7] | rsp[8] << 8);
	log_printf("bt: chip %u.%u.%u, script %s\n", sub >> 10 & 0x1f, (sub >> 7 & 7) + (sub & 0x8000 ? 8 : 0), sub & 0x7f,
		   SCRIPT_NAME);
	if ((r = set_baud(BAUD_FAST)))
		return fail(info, "baud rate", r);
	uint32_t t1 = time_ms();
	if ((r = script_run(script, script_len)))
		return fail(info, "script", r);
	info->patch_ms = time_ms() - t1;
	/* A chip that has low energy on by itself does not know this. */
	if ((r = hci_cmd(TI_LE_ENABLE, (const uint8_t[]){ 1, 1 }, 2, NULL, NULL, 1000)))
		log_printf("bt: low energy enable not taken: %d\n", r);
	if (bdaddr && (r = write_bdaddr(bdaddr)))
		return fail(info, "write bd_addr", r);
	n = 9;
	if (!hci_cmd(HCI_READ_LOCAL_VERSION, NULL, 0, rsp, &n, 1000) && n >= 9)
		info->lmp_subversion = (uint16_t)(rsp[7] | rsp[8] << 8);
	if ((r = read_bdaddr(info->bdaddr)))
		return fail(info, "read bd_addr", r);
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
	log_printf("bt: up in %lu ms (script %lu ms), lmp subversion %04x, uart %lu/%lu/%lu, hci %lu/%lu\n", info->total_ms,
		   info->patch_ms, info->lmp_subversion, rx, tx, uerr, pkts, herr);
	wake_pin(WAKE_PAD, true, false);
	return 0;
}

void bt_stop(void)
{
	wake_pin(WAKE_PAD, false, false);
	uart_stop(BT_UART);
	power(false);
	running = false;
	chip = AWAKE;
}

bool bt_running(void) { return running; }

/* There is no line for it: whether the chip is awake. */
bool bt_host_wake(void) { return running && chip == AWAKE; }

/* The chip goes to sleep by itself and says so. */
void bt_lpm_poll(void) {}
uint32_t bt_lpm_next_ms(void) { return UINT32_MAX; }

bool bt_can_sleep(void)
{
	return !running || (chip == ASLEEP && uart_idle(BT_UART));
}

/* The UART keeps its clock while the watch sleeps, so a byte from the
 * chip is what wakes it. */
void bt_sleep(bool asleep) {}
