/* USB device: the ChipIdea high-speed controller behind the ULPI PHY the
 * bootloader already brought up. The firmware re-enables the clocks,
 * resets the controller, keeps the PHY on external VBUS sensing (as the
 * bootloader runs it), and lets TinyUSB drive it. The gadget is a
 * CDC-ACM console plus an ADB interface (class ff/42/01) whose bulk
 * endpoints are handled by a small class driver: every ADB header and
 * payload is its own transfer in both directions. */
#include <string.h>
#include "asteroid.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "usb/adb.h"
#include "console/console.h"

#define ULPI_RUN		BIT(30)
#define ULPI_WRITE		BIT(29)
#define ULPI_MISC_A_SET		0x97
#define ULPI_MISC_A_VBUSVLDEXTSEL BIT(1)
#define ULPI_MISC_A_VBUSVLDEXT	BIT(0)
#define GEN2_SESS_VLD_CTRL_EN	BIT(7)
#define USBCMD_SESS_VLD_CTRL	BIT(25)
#define PORTSC_PTS_ULPI		BIT(31)
#define PORTSC_CCS		BIT(0)
#define PORTSC_SUSP		BIT(7)

#define EP_ADB_OUT	0x01
#define EP_ADB_IN	0x81
#define EP_ACM_NOTIFY	0x83
#define EP_ACM_OUT	0x04
#define EP_ACM_IN	0x84
enum { ITF_ACM_COMM = 0, ITF_ACM_DATA, ITF_ADB, ITF_COUNT };

static void ulpi_write(uint8_t reg, uint8_t val)
{
	writel(ULPI_RUN | ULPI_WRITE | (uint32_t)reg << 16 | val, USB_ULPI_VIEWPORT);
	while (readl(USB_ULPI_VIEWPORT) & ULPI_RUN)
		;
}

/* The PHY selection and the external session-valid wiring survive
 * neither a controller reset nor the stack's own reset, so they are
 * applied before and after the stack initialises. */
static void phy_select(void)
{
	writel(PORTSC_PTS_ULPI, USB_PORTSC);
	ulpi_write(ULPI_MISC_A_SET, ULPI_MISC_A_VBUSVLDEXTSEL | ULPI_MISC_A_VBUSVLDEXT);
	writel(readl(USB_GENCONFIG_2) | GEN2_SESS_VLD_CTRL_EN, USB_GENCONFIG_2);
	writel(readl(USB_USBCMD) | USBCMD_SESS_VLD_CTRL, USB_USBCMD);
}

static void controller_init(void)
{
	writel(readl(GCC_USB_HS_AHB_CBCR) | 1, GCC_USB_HS_AHB_CBCR);
	writel(readl(GCC_USB_HS_SYSTEM_CBCR) | 1, GCC_USB_HS_SYSTEM_CBCR);
	delay_ms(1);
	writel(0x00080002, USB_USBCMD);
	delay_ms(2);
	while (readl(USB_USBCMD) & 2)
		;
	phy_select();
	writel(0, USB_SBUSCFG);
	writel(0x08, USB_AHB_MODE);
}

/* TinyUSB port hooks */
int tusb_log(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	log_puts("tusb: ");
	log_puts(buf);
	return n;
}
uint32_t tusb_time_millis_api(void) { return time_ms(); }
void tusb_time_delay_ms_api(uint32_t ms) { delay_ms(ms); }
bool dcd_dcache_clean(const void *addr, uint32_t n) { dcache_clean_range(addr, n); return true; }
bool dcd_dcache_invalidate(const void *addr, uint32_t n) { dcache_invalidate_range(addr, n); return true; }
bool dcd_dcache_clean_invalidate(const void *addr, uint32_t n) { dcache_clean_invalidate_range(addr, n); return true; }
static void usb_irq(void) { dcd_int_handler(0); }

/* Descriptors */
static const tusb_desc_device_t desc_device = {
	.bLength = sizeof(tusb_desc_device_t),
	.bDescriptorType = TUSB_DESC_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = TUSB_CLASS_MISC,
	.bDeviceSubClass = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor = 0x18d1,
	.idProduct = 0x4ee7,
	.bcdDevice = 0x0100,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};

#define ADB_DESC_LEN 23
#define ADB_DESCRIPTOR(itf, epin, epout, epsize) \
	9, TUSB_DESC_INTERFACE, itf, 0, 2, 0xff, 0x42, 0x01, 0, \
	7, TUSB_DESC_ENDPOINT, epin, TUSB_XFER_BULK, U16_TO_U8S_LE(epsize), 0, \
	7, TUSB_DESC_ENDPOINT, epout, TUSB_XFER_BULK, U16_TO_U8S_LE(epsize), 0
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + ADB_DESC_LEN)

static const uint8_t desc_config_hs[] = {
	TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, 500),
	TUD_CDC_DESCRIPTOR(ITF_ACM_COMM, 0, EP_ACM_NOTIFY, 10, EP_ACM_OUT, EP_ACM_IN, 512),
	ADB_DESCRIPTOR(ITF_ADB, EP_ADB_IN, EP_ADB_OUT, 512),
};
static const uint8_t desc_config_fs[] = {
	TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, 500),
	TUD_CDC_DESCRIPTOR(ITF_ACM_COMM, 0, EP_ACM_NOTIFY, 10, EP_ACM_OUT, EP_ACM_IN, 64),
	ADB_DESCRIPTOR(ITF_ADB, EP_ADB_IN, EP_ADB_OUT, 64),
};
static const tusb_desc_device_qualifier_t desc_qualifier = {
	.bLength = sizeof(tusb_desc_device_qualifier_t),
	.bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
	.bcdUSB = 0x0200,
	.bDeviceClass = TUSB_CLASS_MISC,
	.bDeviceSubClass = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
	.bNumConfigurations = 1,
};
static char serial[32] = "asteroid";

uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&desc_device; }
uint8_t const *tud_descriptor_device_qualifier_cb(void) { return (uint8_t const *)&desc_qualifier; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	return tud_speed_get() == TUSB_SPEED_HIGH ? desc_config_hs : desc_config_fs;
}
uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
	return tud_speed_get() == TUSB_SPEED_HIGH ? desc_config_fs : desc_config_hs;
}

static uint16_t desc_str[48];
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	const char *s;
	switch (index) {
	case 0: desc_str[1] = 0x0409; desc_str[0] = 0x0300 | 4; return desc_str;
	case 1: s = "AsteroidOS"; break;
	case 2: s = "asteroid-embedded"; break;
	case 3: s = serial; break;
	default: return NULL;
	}
	unsigned n = strlen(s);
	if (n > 31) n = 31;
	for (unsigned i = 0; i < n; i++)
		desc_str[1 + i] = (uint8_t)s[i];
	desc_str[0] = (uint16_t)(0x0300 | (2 * n + 2));
	return desc_str;
}

/* ADB interface: a class driver that owns the two bulk endpoints. */
static uint8_t adb_rx_buf[16384] __attribute__((aligned(64)));
static uint8_t adb_tx_buf[16384 + 64] __attribute__((aligned(64)));
static bool adb_open, adb_tx_busy, adb_rx_queued;
static uint32_t usb_rx_bytes, usb_tx_bytes;

static void adb_queue_rx(void)
{
	if (!adb_open || adb_rx_queued)
		return;
	uint32_t n = adb_rx_expected();
	if (n > sizeof adb_rx_buf)
		n = sizeof adb_rx_buf;
	if (!n)
		n = 24;
	adb_rx_queued = usbd_edpt_xfer(0, EP_ADB_OUT, adb_rx_buf, (uint16_t)n, false);
}

static bool adb_send(const void *buf, uint32_t len)
{
	if (!adb_open || adb_tx_busy || len > sizeof adb_tx_buf - 64)
		return false;
	memcpy(adb_tx_buf, buf, len);
	adb_tx_busy = usbd_edpt_xfer(0, EP_ADB_IN, adb_tx_buf, (uint16_t)len, false);
	return adb_tx_busy;
}

static void adb_drv_init(void) {}
static void adb_drv_reset(uint8_t rhport)
{
	adb_open = adb_tx_busy = adb_rx_queued = false;
	adb_reset();
}
static uint16_t adb_drv_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
	if (itf->bInterfaceClass != 0xff || itf->bInterfaceSubClass != 0x42 || itf->bInterfaceProtocol != 0x01)
		return 0;
	const uint8_t *p = (const uint8_t *)itf + itf->bLength;
	for (int i = 0; i < 2; i++) {
		usbd_edpt_open(rhport, (tusb_desc_endpoint_t const *)p);
		p += p[0];
	}
	adb_open = true;
	adb_queue_rx();
	return (uint16_t)(p - (const uint8_t *)itf);
}
static bool adb_drv_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) { return false; }
static bool adb_drv_xfer(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t n)
{
	if (ep == EP_ADB_OUT) {
		adb_rx_queued = false;
		if (result == XFER_RESULT_SUCCESS) {
			usb_rx_bytes += n;
			adb_rx(adb_rx_buf, n);
		}
		adb_queue_rx();
	} else if (ep == EP_ADB_IN) {
		adb_tx_busy = false;
		usb_tx_bytes += n;
	}
	return true;
}

static const usbd_class_driver_t adb_driver = {
	.name = "adb",
	.init = adb_drv_init,
	.reset = adb_drv_reset,
	.open = adb_drv_open,
	.control_xfer_cb = adb_drv_control,
	.xfer_cb = adb_drv_xfer,
};
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *count)
{
	*count = 1;
	return &adb_driver;
}

/* CDC console. Output from inside a TinyUSB callback cannot wait for
 * the host (tud_task would nest), so it is dropped when the buffer is
 * full there; commands run from the main loop, where it can wait. */
static bool cdc_in_cb;

static void cdc_out(const char *s, size_t n, void *ctx)
{
	while (n) {
		uint32_t k = tud_cdc_write(s, (uint32_t)n);
		if (!k) {
			if (cdc_in_cb)
				return;
			tud_cdc_write_flush();
			tud_task();
			if (!tud_cdc_connected())
				return;
			continue;
		}
		s += k;
		n -= k;
	}
}

void tud_cdc_rx_cb(uint8_t itf)
{
	char buf[64];
	uint32_t n;
	cdc_in_cb = true;
	while ((n = tud_cdc_read(buf, sizeof buf)) > 0)
		console_input(buf, n);
	cdc_in_cb = false;
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
	cdc_in_cb = true;
	if (dtr)
		console_attach(cdc_out, NULL, true);
	else
		console_detach();
	cdc_in_cb = false;
	log_printf("usb: console %s\n", dtr ? "opened" : "closed");
}

void tud_mount_cb(void)
{
	log_printf("usb: configured, %s speed\n", tud_speed_get() == TUSB_SPEED_HIGH ? "high" : "full");
}
void tud_umount_cb(void) { log_printf("usb: unconfigured\n"); }
/* A host is attached and the bus is not suspended. This comes from the
 * port itself, as the controller only raises its interrupts while the
 * crystal runs: after an unplug the bus goes idle and the port suspends,
 * a newly attached host resets the port out of suspend. */
bool usb_connected(void)
{
	return (readl(USB_PORTSC) & (PORTSC_CCS | PORTSC_SUSP)) == PORTSC_CCS;
}

static const struct adb_transport transport = { .send = adb_send, .pump = usb_poll };

void usb_init(const char *serialno)
{
	unsigned n = 0;
	while (serialno[n] && n < sizeof serial - 1) {
		serial[n] = serialno[n];
		n++;
	}
	serial[n] = 0;
	controller_init();
	gic_enable(IRQ_USB_HS, usb_irq);
	gic_mask(IRQ_USB_HS);
	adb_init(&transport, "ro.product.name=" BOARD_NAME ";ro.product.model=asteroid-embedded;ro.product.device=" BOARD_NAME);
	tusb_rhport_init_t rh = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_HIGH };
	if (!tusb_init(0, &rh)) {
		log_printf("usb: stack init failed\n");
		return;
	}
	tud_disconnect();
	phy_select();
	tud_connect();
}

void usb_poll(void)
{
	tud_task();
	adb_poll();
	if (tud_cdc_connected())
		tud_cdc_write_flush();
	adb_queue_rx();
}

void usb_stats(uint32_t *rx, uint32_t *tx) { *rx = usb_rx_bytes; *tx = usb_tx_bytes; }
