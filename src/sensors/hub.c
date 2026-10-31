/* Sensor hub: an STM32 that owns every motion, pressure and heart rate
 * sensor, on BLSP1 UART1 at 115200 8N1 without flow control. Frames are
 * HDLC-like: 0x7d/0x7e escaped as 0x7d, c ^ 0x20, a CRC-16/X.25 sent low
 * byte first, then a single 0x7e. The payload starts with a channel byte:
 * channel 0 carries [0, type, cmd], channel 1 [1, type, tag, cmd], with
 * type 0 a request, 1 its response and 2 an event. Its BOOT0/BOOT1 pins
 * select the ROM bootloader on reset, so they are held low throughout.
 * After the handshake the MCU only sends while gpio 108 says the AP is
 * awake: otherwise it raises gpio 65 and waits about 150 ms for 108, then
 * drops the message. 108 therefore goes low only while the cluster is off,
 * with gpio 65 (MPM pin 29) as a wake-up source, and the cluster stays on
 * while 65 is high and for a while after, until the message is in. The
 * sensors only start with their calibration set, which comes from the
 * nvdata partition: the accelerometer offsets as three int16 and the
 * pressure offset as text. */
#include <string.h>
#include "asteroid.h"
#include "hub.h"
#include "storage/storage.h"

#define UART		BOARD_HUB_UART
#define GPIO_UART	BOARD_HUB_GPIO
#define FUNC_UART	BOARD_HUB_FUNC
#define GPIO_WAKE_MCU	BOARD_HUB_WAKE_MCU
#define GPIO_MCU_WAKE	BOARD_HUB_MCU_WAKE
#define GPIO_RESET	BOARD_HUB_RESET
#define GPIO_BOOT1	BOARD_HUB_BOOT1
#define GPIO_AP_STATUS	BOARD_HUB_AP_STATUS
#define GPIO_BOOT0	BOARD_HUB_BOOT0
#define GPIO_MCU_STATUS	BOARD_HUB_MCU_STATUS

#define RESET_MS	20
#define BOOT_MS		500
#define REPLY_MS	1000
#define TRIES		5
#define MPM_PIN_MCU_WAKE	BOARD_HUB_MPM
#define WAKE_HOLD_MS	300

enum { CH_CONTROL, CH_SERVICE, CH_LOG = 4 };
enum { T_REQUEST, T_RESPONSE, T_EVENT };
#define TAG_VERSION	0x0f
#define TAG_WRIST_TILT	0x33
#define CMD_ACTIVATE	0x00
#define CMD_BATCH	0x03
#define CMD_PARAMETER	0x13
#define TILT_PERIOD_MS	10
#define CMD_EVENT	0x10

enum state { S_OFF, S_BOOT, S_VERSION, S_HANDSHAKE, S_PARAMETER, S_TILT, S_TILT_BATCH, S_READY, S_FAILED };

static enum state state;
static uint32_t state_t;
static unsigned tries;
static uint8_t rx[1100];
static unsigned rx_len;
static bool rx_esc;
static uint32_t frames, bad_frames;
static char version[49];
static bool tilted;
static unsigned parameter;
static uint32_t wake_t, mcu_wakes;

/* Tag, then up to 6 bytes of value: accelerometer, pressure, a fixed 5
 * for tag 0x17, gyro. */
static uint8_t parameters[][8] = {
	{ 0x10, 6 }, { 0x16, 4 }, { 0x17, 4, 5 }, { 0x11, 4 },
};
static uint32_t tilts;

static uint16_t crc16(const uint8_t *p, unsigned n)
{
	uint16_t c = 0xffff;
	while (n--) {
		c ^= *p++;
		for (int i = 0; i < 8; i++)
			c = c & 1 ? (c >> 1) ^ 0x8408 : c >> 1;
	}
	return c;
}

static unsigned escape(uint8_t *out, uint8_t c)
{
	if (c == 0x7d || c == 0x7e) {
		out[0] = 0x7d;
		out[1] = c ^ 0x20;
		return 2;
	}
	out[0] = c;
	return 1;
}

static int send(const uint8_t *p, unsigned n)
{
	uint8_t buf[2 * 36 + 5];
	if (n > 36)
		return -1;
	uint16_t c = crc16(p, n) ^ 0xffff;
	unsigned k = 0;
	for (unsigned i = 0; i < n; i++)
		k += escape(buf + k, p[i]);
	k += escape(buf + k, (uint8_t)c);
	k += escape(buf + k, (uint8_t)(c >> 8));
	buf[k++] = 0x7e;
	return uart_dm_write(UART, buf, k);
}

static void enter(enum state s)
{
	state = s;
	state_t = time_ms();
	tries = 0;
}

static void load_calibration(void)
{
	static struct ext4 fs;
	struct ext4_inode in;
	uint32_t first, count;
	char text[16] = { 0 };
	int16_t acc[3] = { 0, 0, 0 };
	if ((!emmc_part_count() && emmc_part_scan()) || emmc_part_find("nvdata", &first, &count) ||
	    ext4_open_lba(&fs, "nvdata", first, count)) {
		log_printf("hub: no nvdata, sensors uncalibrated\n");
		return;
	}
	if (!ext4_lookup(&fs, "/accclibration", &in) && ext4_read(&fs, &in, 0, acc, sizeof acc) == sizeof acc &&
	    acc[0] >= -312 && acc[0] <= 312 && acc[1] >= -312 && acc[1] <= 312 && acc[2] >= -312 && acc[2] <= 312)
		memcpy(&parameters[0][2], acc, sizeof acc);
	else
		acc[0] = acc[1] = acc[2] = 0;
	if (!ext4_lookup(&fs, "/BARO", &in) && ext4_read(&fs, &in, 0, text, sizeof text - 1) > 0) {
		int32_t baro = text[0] == '-' ? -(int32_t)strtoul(text + 1, NULL, 10) : (int32_t)strtoul(text, NULL, 10);
		if (baro > -499 && baro < 500)
			memcpy(&parameters[1][2], &baro, sizeof baro);
	}
	log_printf("hub: calibration accel %d %d %d\n", acc[0], acc[1], acc[2]);
}

static void request(void)
{
	static const uint8_t version_req[] = { CH_SERVICE, T_REQUEST, TAG_VERSION, 0 };
	static const uint8_t handshake[] = { CH_CONTROL, T_REQUEST, 0 };
	static const uint8_t tilt_on[] = { CH_SERVICE, T_REQUEST, TAG_WRIST_TILT, CMD_ACTIVATE };
	static const uint8_t tilt_batch[] = { CH_SERVICE, T_REQUEST, TAG_WRIST_TILT, CMD_BATCH, TILT_PERIOD_MS, 0, 0, 0,
					      0, 0, 0, 0, 0, 0, 0, 0 };
	if (state == S_VERSION)
		send(version_req, sizeof version_req);
	else if (state == S_HANDSHAKE)
		send(handshake, sizeof handshake);
	else if (state == S_PARAMETER) {
		uint8_t m[4 + 6] = { CH_SERVICE, T_REQUEST, parameters[parameter][0], CMD_PARAMETER };
		memcpy(m + 4, &parameters[parameter][2], parameters[parameter][1]);
		send(m, 4 + parameters[parameter][1]);
	}
	else if (state == S_TILT)
		send(tilt_on, sizeof tilt_on);
	else
		send(tilt_batch, sizeof tilt_batch);
	tries++;
	state_t = time_ms();
}

static void dump(const char *what, const uint8_t *p, unsigned n)
{
	char hex[3 * 16 + 1];
	unsigned k = 0;
	for (unsigned i = 0; i < n && i < 16; i++)
		k += (unsigned)snprintf(hex + k, sizeof hex - k, " %02x", p[i]);
	hex[k] = 0;
	log_printf("hub: %s %u bytes:%s%s\n", what, n, hex, n > 16 ? " ..." : "");
}

static void frame(const uint8_t *p, unsigned n)
{
	if (n < 3 || crc16(p, n) != 0xf0b8) {
		bad_frames++;
		if (n > 1)
			dump("bad frame", p, n);
		return;
	}
	n -= 2;
	frames++;
	if (p[0] == CH_LOG) {
		char text[96];
		unsigned len = n - 1 < sizeof text - 1 ? n - 1 : sizeof text - 1;
		memcpy(text, p + 1, len);
		text[len] = 0;
		log_printf("hub: mcu: %s\n", text);
		return;
	}
	if (state == S_VERSION && n >= 4 && p[0] == CH_SERVICE && p[1] == T_RESPONSE && p[2] == TAG_VERSION) {
		unsigned len = n - 4 < sizeof version - 1 ? n - 4 : sizeof version - 1;
		memcpy(version, p + 4, len);
		version[len] = 0;
		log_printf("hub: version %s\n", version);
		enter(S_HANDSHAKE);
		request();
		return;
	}
	if (state == S_HANDSHAKE && n >= 3 && p[0] == CH_CONTROL && p[1] == T_RESPONSE && p[2] == 0) {
		log_printf("hub: handshake done\n");
		spm_wake_pin(MPM_PIN_MCU_WAKE, true, true);
		load_calibration();
		parameter = 0;
		enter(S_PARAMETER);
		request();
		return;
	}
	if (state == S_PARAMETER && n >= 5 && p[0] == CH_SERVICE && p[1] == T_RESPONSE &&
	    p[2] == parameters[parameter][0] && p[3] == CMD_PARAMETER) {
		if (p[4])
			log_printf("hub: parameter %02x failed (%u)\n", p[2], p[4]);
		if (++parameter < ARRAY_SIZE(parameters)) {
			tries = 0;
			request();
		} else {
			enter(S_TILT);
			request();
		}
		return;
	}
	if (state == S_TILT && n >= 5 && p[0] == CH_SERVICE && p[1] == T_RESPONSE && p[2] == TAG_WRIST_TILT &&
	    p[3] == CMD_ACTIVATE) {
		log_printf("hub: wrist tilt %s (%u)\n", p[4] ? "failed" : "on", p[4]);
		enter(S_TILT_BATCH);
		request();
		return;
	}
	if (state == S_TILT_BATCH && n >= 5 && p[0] == CH_SERVICE && p[1] == T_RESPONSE && p[2] == TAG_WRIST_TILT &&
	    p[3] == CMD_BATCH) {
		log_printf("hub: wrist tilt every %u ms (%u)\n", TILT_PERIOD_MS, p[4]);
		enter(S_READY);
		return;
	}
	if (n >= 4 && p[0] == CH_SERVICE && p[1] == T_EVENT && p[2] == TAG_WRIST_TILT && p[3] == CMD_EVENT) {
		log_printf("hub: wrist tilt\n");
		tilted = true;
		tilts++;
		return;
	}
	dump("frame", p, n);
}

static void receive(void)
{
	uint8_t buf[64];
	unsigned n;
	while ((n = uart_dm_read(UART, buf, sizeof buf)))
		for (unsigned i = 0; i < n; i++) {
			uint8_t c = buf[i];
			if (c == 0x7e) {
				frame(rx, rx_len);
				rx_len = 0;
				rx_esc = false;
			} else if (c == 0x7d) {
				rx_esc = true;
			} else if (rx_len < sizeof rx) {
				rx[rx_len++] = rx_esc ? c ^ 0x20 : c;
				rx_esc = false;
			}
		}
}

bool hub_init(void)
{
	tlmm_config(GPIO_BOOT0, 0, TLMM_OUTPUT, TLMM_PULL_DOWN, 2);
	tlmm_set(GPIO_BOOT0, 0);
	tlmm_config(GPIO_BOOT1, 0, TLMM_OUTPUT, TLMM_PULL_DOWN, 2);
	tlmm_set(GPIO_BOOT1, 0);
	tlmm_config(GPIO_WAKE_MCU, 0, TLMM_OUTPUT, TLMM_PULL_DOWN, 2);
	tlmm_set(GPIO_WAKE_MCU, 0);
	tlmm_config(GPIO_AP_STATUS, 0, TLMM_OUTPUT, TLMM_PULL_DOWN, 2);
	tlmm_set(GPIO_AP_STATUS, 1);
	tlmm_config(GPIO_MCU_WAKE, 0, TLMM_INPUT, TLMM_PULL_DOWN, 2);
	tlmm_config(GPIO_MCU_STATUS, 0, TLMM_INPUT, TLMM_PULL_DOWN, 2);
	tlmm_config(GPIO_RESET, 0, TLMM_OUTPUT, TLMM_PULL_UP, 2);
	tlmm_set(GPIO_RESET, 0);
	delay_ms(RESET_MS);
	tlmm_set(GPIO_RESET, 1);
	if (uart_dm_init(UART, GPIO_UART, FUNC_UART, 115200, false)) {
		enter(S_FAILED);
		return false;
	}
	enter(S_BOOT);
	return true;
}

void hub_poll(void)
{
	if (state == S_OFF || state == S_FAILED)
		return;
	receive();
	uint32_t now = time_ms();
	if (tlmm_get(GPIO_MCU_WAKE))
		wake_t = now;
	if (state == S_BOOT && now - state_t >= BOOT_MS) {
		enter(S_VERSION);
		request();
	} else if (state >= S_VERSION && state <= S_TILT_BATCH && now - state_t >= REPLY_MS) {
		if (tries == TRIES) {
			log_printf("hub: no %s reply, mcu status %u\n",
				   state == S_VERSION ? "version" : state == S_HANDSHAKE ? "handshake" :
				   state == S_PARAMETER ? "parameter" : "wrist tilt",
				   tlmm_get(GPIO_MCU_STATUS));
			enter(S_FAILED);
		} else {
			request();
		}
	}
}

int hub_send(const uint8_t *p, unsigned n)
{
	return state == S_READY ? send(p, n) : -1;
}

bool hub_tilted(void)
{
	bool t = tilted;
	tilted = false;
	return t;
}

bool hub_can_sleep(void)
{
	return state != S_READY || (!tlmm_get(GPIO_MCU_WAKE) && time_ms() - wake_t >= WAKE_HOLD_MS);
}

void hub_sleep(bool asleep)
{
	if (state >= S_TILT && state <= S_READY)
		tlmm_set(GPIO_AP_STATUS, !asleep);
	if (!asleep && tlmm_get(GPIO_MCU_WAKE)) {
		wake_t = time_ms();
		mcu_wakes++;
	}
}

void hub_status(void (*say)(const char *fmt, ...))
{
	static const char *const names[] = { "off", "booting", "version", "handshake", "parameter", "wrist tilt",
					     "tilt batch", "ready", "failed" };
	uint32_t rxb, txb, err;
	uart_dm_stats(UART, &rxb, &txb, &err);
	say("hub: %s, version %s, %lu tilts, %lu mcu wakes, %lu frames, %lu bad, uart %lu/%lu/%lu, mcu status %u wake %u\n",
	    names[state], version[0] ? version : "?", tilts, mcu_wakes, frames, bad_frames, rxb, txb, err, tlmm_get(GPIO_MCU_STATUS),
	    tlmm_get(GPIO_MCU_WAKE));
}
