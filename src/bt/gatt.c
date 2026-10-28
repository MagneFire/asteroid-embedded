/* The GATT services asteroid-btsyncd exposes and AsteroidOSSync talks to:
 * vendor services on the base xxxxxxxx-0000-0000-0000-00a57e401d05 plus
 * the standard Battery service, with btsyncd's properties (writes and
 * reads need an authenticated, encrypted link). Battery and time are
 * real; the rest log what the phone sends. */
#include <string.h>
#include "asteroid.h"
#include "host/ble_hs.h"
#include "bt.h"
#include "notify/notify.h"

#define ASTEROID_UUID(n) \
	BLE_UUID128_INIT(0x05, 0x1d, 0x40, 0x7e, 0xa5, 0, 0, 0, 0, 0, 0, 0, (n) & 0xff, (n) >> 8, 0, 0)
#define UUID128(n) (&((const ble_uuid128_t)ASTEROID_UUID(n)).u)
#define UUID16(n) (&((const ble_uuid16_t)BLE_UUID16_INIT(n)).u)

#define AUTH_WRITE (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN)
#define AUTH_READ (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN)

#define CHR_TIME_SET		0x5001
#define CHR_SCREENSHOT_REQ	0x6001
#define CHR_SCREENSHOT_DATA	0x6002
#define CHR_MEDIA_COMMANDS	0x7005
#define CHR_NOTIF_UPDATE	0x9001
#define CHR_NOTIF_FEEDBACK	0x9002
#define CHR_BATTERY_LEVEL	0x2a19

const ble_uuid128_t bt_asteroid_adv_uuid = ASTEROID_UUID(0x0000);

static uint16_t h_time, h_screenshot, h_media, h_weather_city, h_notif_feedback, h_battery;
static uint8_t battery_sent = 0xff;
static uint8_t payload[1024];

static uint8_t battery_percent(void)
{
	uint32_t p = pmic_battery_percent(pmic_battery_mv());
	return (uint8_t)(p > 100 ? 100 : p);
}

static const char *chr_name(uint16_t id)
{
	switch (id) {
	case 0x7001: return "media title";
	case 0x7002: return "media album";
	case 0x7003: return "media artist";
	case 0x7004: return "media playing";
	case 0x7006: return "media volume";
	case 0x8001: return "weather city";
	case 0x8002: return "weather ids";
	case 0x8003: return "weather min";
	case 0x8004: return "weather max";
	case CHR_SCREENSHOT_REQ: return "screenshot request";
	default: return "write";
	}
}

static void log_payload(const char *what, const uint8_t *p, unsigned n)
{
	char text[97];
	log_printf("bt: %s, %u bytes\n", what, n);
	for (unsigned off = 0; off < n; off += sizeof text - 1) {
		unsigned k = n - off < sizeof text - 1 ? n - off : sizeof text - 1;
		for (unsigned i = 0; i < k; i++)
			text[i] = p[off + i] >= 0x20 && p[off + i] < 0x7f ? (char)p[off + i] : '.';
		text[k] = 0;
		log_printf("bt:   %s\n", text);
	}
}

static int set_time(const uint8_t *p, unsigned n)
{
	if (n < 6)
		return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
	struct civil c = { 1900u + p[0], 1u + p[1], p[2], 0, p[3], p[4], p[5] };
	if (c.month > 12 || !c.day || c.day > 31 || c.hour > 23 || c.minute > 59 || c.second > 60)
		return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
	uint32_t secs = epoch_from_civil(&c);
	pmic_rtc_write(secs);
	log_printf("bt: time set to %04lu-%02lu-%02lu %02lu:%02lu:%02lu (rtc %lu)\n", c.year, c.month, c.day, c.hour,
		   c.minute, c.second, secs);
	return 0;
}

static int access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	uint16_t id = (uint16_t)(uintptr_t)arg;
	if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
		if (id == CHR_BATTERY_LEVEL) {
			uint8_t v = battery_percent();
			battery_sent = v;
			return os_mbuf_append(ctxt->om, &v, 1) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
		}
		if (id == CHR_SCREENSHOT_DATA)
			return 0;
		return BLE_ATT_ERR_READ_NOT_PERMITTED;
	}
	if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
		return BLE_ATT_ERR_UNLIKELY;
	uint16_t n = 0;
	if (ble_hs_mbuf_to_flat(ctxt->om, payload, sizeof payload, &n))
		return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
	if (id == CHR_TIME_SET)
		return set_time(payload, n);
	if (id == CHR_NOTIF_UPDATE) {
		notify_feed(payload, n);
		return 0;
	}
	log_payload(chr_name(id), payload, n);
	return 0;
}

#define CHR(n, f, h) { .uuid = UUID128(n), .access_cb = access, .arg = (void *)(n), .flags = (f), .val_handle = (h) }

static const struct ble_gatt_svc_def services[] = {
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = UUID128(0x5071),
		.characteristics = (const struct ble_gatt_chr_def[]){
			CHR(CHR_TIME_SET, AUTH_WRITE | BLE_GATT_CHR_F_NOTIFY, &h_time),
			{ 0 },
		},
	},
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = UUID128(0x6071),
		.characteristics = (const struct ble_gatt_chr_def[]){
			CHR(CHR_SCREENSHOT_REQ, AUTH_WRITE, NULL),
			CHR(CHR_SCREENSHOT_DATA, AUTH_READ | BLE_GATT_CHR_F_NOTIFY, &h_screenshot),
			{ 0 },
		},
	},
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = UUID128(0x7071),
		.characteristics = (const struct ble_gatt_chr_def[]){
			CHR(0x7001, AUTH_WRITE, NULL),
			CHR(0x7002, AUTH_WRITE, NULL),
			CHR(0x7003, AUTH_WRITE, NULL),
			CHR(0x7004, AUTH_WRITE, NULL),
			CHR(CHR_MEDIA_COMMANDS, BLE_GATT_CHR_F_NOTIFY, &h_media),
			CHR(0x7006, AUTH_WRITE, NULL),
			{ 0 },
		},
	},
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = UUID128(0x8071),
		.characteristics = (const struct ble_gatt_chr_def[]){
			CHR(0x8001, AUTH_WRITE | BLE_GATT_CHR_F_NOTIFY, &h_weather_city),
			CHR(0x8002, AUTH_WRITE, NULL),
			CHR(0x8004, AUTH_WRITE, NULL),
			CHR(0x8003, AUTH_WRITE, NULL),
			{ 0 },
		},
	},
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = UUID128(0x9071),
		.characteristics = (const struct ble_gatt_chr_def[]){
			CHR(CHR_NOTIF_UPDATE, AUTH_WRITE, NULL),
			CHR(CHR_NOTIF_FEEDBACK, BLE_GATT_CHR_F_NOTIFY, &h_notif_feedback),
			{ 0 },
		},
	},
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = UUID16(0x180f),
		.characteristics = (const struct ble_gatt_chr_def[]){
			{
				.uuid = UUID16(CHR_BATTERY_LEVEL),
				.access_cb = access,
				.arg = (void *)CHR_BATTERY_LEVEL,
				.flags = AUTH_READ | BLE_GATT_CHR_F_NOTIFY,
				.val_handle = &h_battery,
			},
			{ 0 },
		},
	},
	{ 0 },
};

int bt_gatt_init(void)
{
	int r = ble_gatts_count_cfg(services);
	return r ? r : ble_gatts_add_svcs(services);
}

void bt_gatt_battery_check(void)
{
	uint8_t v = battery_percent();
	if (v != battery_sent) {
		battery_sent = v;
		ble_gatts_chr_updated(h_battery);
	}
}
