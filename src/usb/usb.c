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
#include "config.h"

#define ULPI_RUN		BIT(30)
#define ULPI_WRITE		BIT(29)
#define ULPI_MISC_A_SET		0x97
#define ULPI_MISC_A_CLR		0x98
#define ULPI_PHY_OVERRIDE	0x80
#define ULPI_MISC_A_VBUSVLDEXTSEL BIT(1)
#define ULPI_MISC_A_VBUSVLDEXT	BIT(0)
#define GEN2_SESS_VLD_CTRL_EN	BIT(7)
#define USBCMD_SESS_VLD_CTRL	BIT(25)
#define USBCMD_ASYNC_INTR	BIT(29)
#define USBCMD_STP		BIT(30)
#define PORTSC_PTS_ULPI		BIT(31)
#define PORTSC_CCS		BIT(0)
#define PORTSC_SUSP		BIT(7)
#define PORTSC_PHCD		BIT(23)

#define EP_ADB_OUT	0x01
#define EP_ADB_IN	0x81
#define EP_ACM_NOTIFY	0x83
#define EP_ACM_OUT	0x04
#define EP_ACM_IN	0x84
enum { ITF_ACM_COMM = 0, ITF_ACM_DATA, ITF_ADB, ITF_COUNT };
static bool cdc_attached;
/* usb_down: the stack is stopped. usb_clocks_off: both controller clocks are
 * off, so no USB register may be touched. rest_gave_up: unplugged, the PHY
 * still refused low power mode, so the clocks stay on until the next cable. */
static bool usb_down, usb_clocks_off, rest_gave_up;

static int ulpi_write(uint8_t reg, uint8_t val)
{
	writel(ULPI_RUN | ULPI_WRITE | (uint32_t)reg << 16 | val, USB_ULPI_VIEWPORT);
	uint32_t t0 = time_us();
	while (readl(USB_ULPI_VIEWPORT) & ULPI_RUN)
		if (time_us() - t0 > 10000) {
			log_printf("usb: ulpi write %02x timed out\n", reg);
			return -1;
		}
	return 0;
}

/* The PHY selection and the external session-valid wiring survive
 * neither a controller reset nor the stack's own reset, so they are
 * applied before and after the stack initialises. */
static int phy_select(void)
{
	writel(PORTSC_PTS_ULPI, USB_PORTSC);
	int r = ulpi_write(ULPI_MISC_A_SET, ULPI_MISC_A_VBUSVLDEXTSEL | ULPI_MISC_A_VBUSVLDEXT);
	writel(readl(USB_GENCONFIG_2) | GEN2_SESS_VLD_CTRL_EN, USB_GENCONFIG_2);
	writel(readl(USB_USBCMD) | USBCMD_SESS_VLD_CTRL, USB_USBCMD);
	return r;
}

static bool bits_wait(uint32_t addr, uint32_t mask, uint32_t want)
{
	uint32_t t0 = time_ms();
	wdt_feed();
	while ((readl(addr) & mask) != want)
		if (time_ms() - t0 > 500)
			return false;
	return true;
}

static bool controller_init(void)
{
	writel(readl(GCC_USB_HS_AHB_CBCR) | 1, GCC_USB_HS_AHB_CBCR);
	writel(readl(GCC_USB_HS_SYSTEM_CBCR) | 1, GCC_USB_HS_SYSTEM_CBCR);
	delay_ms(1);
	writel(0x00080002, USB_USBCMD);
	delay_ms(2);
	bool ok = bits_wait(USB_USBCMD, 2, 0);
	if (phy_select())
		ok = false;
	writel(0, USB_SBUSCFG);
	writel(0x08, USB_AHB_MODE);
	return ok;
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
/* Transfers completed on the adb endpoints, either way. */
static uint32_t adb_xfers;

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

static void adb_state_clear(void)
{
	adb_open = adb_tx_busy = adb_rx_queued = false;
	adb_reset();
}

static void adb_drv_init(void) {}
static void adb_drv_reset(uint8_t rhport) { adb_state_clear(); }
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
	adb_xfers++;
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

static size_t cdc_room(void *ctx)
{
	return tud_cdc_write_available();
}

/* A host that stops reading loses the output after 1 s in all, rather
 * than holding up the main loop: once that is used up, output is
 * dropped at once until usb_pass finds room again. */
static bool cdc_blocked;
static uint32_t cdc_blocked_t;

static void cdc_out(const char *s, size_t n, void *ctx)
{
	while (n) {
		uint32_t k = tud_cdc_write(s, (uint32_t)n);
		if (!k) {
			if (cdc_in_cb)
				return;
			if (!cdc_blocked) {
				cdc_blocked = true;
				cdc_blocked_t = time_ms();
			} else if (time_ms() - cdc_blocked_t > 1000) {
				return;
			}
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
		console_attach(cdc_out, cdc_room, NULL, true);
	else if (cdc_attached)
		console_detach();
	cdc_attached = dtr;
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
	if (usb_down)
		return false;
	return (readl(USB_PORTSC) & (PORTSC_CCS | PORTSC_SUSP)) == PORTSC_CCS;
}

/* Unplugged, the USB block rests as the vendor's suspend leaves it with
 * VBUS sensed by the PMIC: the stack stopped, the PHY in low power mode
 * (PHCD, then STP) and the controller's clocks off. Coming out of that the
 * host does not enumerate the device again, so a cable plugged in brings
 * it up from scratch, as the vendor's reset does: block reset, controller
 * reset, PHY power-on reset around the board's PHY overrides, then the
 * stack. */
#define PHY_POR			BIT(0)

static bool clock_on(uint32_t cbcr)
{
	writel(readl(cbcr) | 1, cbcr);
	for (int i = 0; i < 1000; i++) {
		if (!(readl(cbcr) & BIT(31)))
			return true;
		delay_us(1);
	}
	return false;
}

static void phy_reset(void)
{
	writel(readl(USB_PHY_CTRL) | PHY_POR, USB_PHY_CTRL);
	delay_us(15);
	writel(readl(USB_PHY_CTRL) & ~PHY_POR, USB_PHY_CTRL);
	dsb();
}

#define AHB2AHB_BYPASS		BIT(31)

/* The PHY's supplies, off as the vendor's unplugged collapse has them
 * and on before anything touches the PHY again. */
static bool phy_rail_off[2];
static bool phy_rails(bool on)
{
	static const struct { uint8_t id; uint32_t uv; } rail[2] = { { 10, 1800000 }, { 20, 3075000 } };
	bool ok = true;
	if (!board->usb_rails_off)
		return true;
	for (unsigned i = 0; i < 2; i++) {
		if (phy_rail_off[i] == !on)
			continue;
		if (regulator_on(rail[i].id, rail[i].uv) || (!on && regulator_off(rail[i].id)))
			ok = false;
		else
			phy_rail_off[i] = !on;
	}
	if (!ok)
		log_printf("usb: phy supplies %s not acknowledged\n", on ? "on" : "off");
	else if (on)
		delay_ms(1);
	return ok;
}

static void usb_clocks_gate(void)
{
	writel(readl(GCC_USB_HS_SYSTEM_CBCR) & ~1u, GCC_USB_HS_SYSTEM_CBCR);
	writel(readl(GCC_USB_HS_AHB_CBCR) & ~1u, GCC_USB_HS_AHB_CBCR);
}

/* The PHY to low power mode, then the clocks off; they stay on if the
 * PHY did not get there. The stack has to be stopped already. */
static bool phy_rest(void)
{
	bool phcd = !ulpi_write(ULPI_MISC_A_CLR, ULPI_MISC_A_VBUSVLDEXTSEL | ULPI_MISC_A_VBUSVLDEXT);
	if (phcd) {
		writel(readl(USB_PORTSC) | PORTSC_PHCD, USB_PORTSC);
		phcd = bits_wait(USB_PORTSC, PORTSC_PHCD, PORTSC_PHCD);
	}
#if defined(TEST_USB_NO_LPM)
	/* The first attempt only, so the second one on unplugging works. */
	static bool refused;
	if (!refused) {
		refused = true;
		return false;
	}
#endif
	if (phcd) {
		writel(readl(USB_USBCMD) | USBCMD_STP, USB_USBCMD);
		dsb();
		usb_clocks_gate();
		phy_rails(false);
	}
	usb_clocks_off = phcd;
	return phcd;
}

/* Unplugged, or a controller that got stuck: the stack stops, then the
 * PHY goes to low power mode and the clocks off. */
static void usb_collapse(const char *why)
{
	if (cdc_attached) {
		console_detach();
		cdc_attached = false;
	}
	tud_deinit(0);
	adb_state_clear();
	irq_mask(IRQ_USB_HS);
	usb_down = true;
	rest_gave_up = !phy_rest();
	log_printf("usb: %s, %s\n", why, rest_gave_up ? "phy did not enter low power mode, clocks left on" : "phy in low power mode");
}

/* The stack starts the controller as it initialises; the session wiring
 * has to follow its reset, then the device attaches. Returns what failed,
 * or NULL. */
static const char *stack_up(void)
{
	adb_state_clear();
	tusb_rhport_init_t rh = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_HIGH };
	if (!tusb_init(0, &rh))
		return "stack";
	tud_disconnect();
	if (phy_select())
		return "phy select";
	dsb();
	tud_connect();
	return NULL;
}

/* Stops what a failed start left running. The stack may count as
 * initialised even though its controller driver failed, and only then
 * would a second start really run. */
static void usb_start_failed(const char *fail, uint32_t t0)
{
	tud_deinit(0);
	irq_mask(IRQ_USB_HS);
	usb_down = true;
	if (!usb_clocks_off)
		phy_rest();
	log_printf("usb: bringing up failed at %s after %lu ms, clocks %s\n", fail, time_ms() - t0, usb_clocks_off ? "off" : "on");
}

/* Brings USB up from scratch. On any failure the stack is stopped
 * again and USB rests as it does unplugged; usb_cable tries again
 * later. */
static bool usb_restore(void)
{
	uint32_t t0 = time_ms();
	const char *fail = NULL;
	rest_gave_up = false;
	if (!phy_rails(true)) {
		fail = "phy supplies";
		goto out;
	}
	bool clocks = clock_on(GCC_USB_HS_AHB_CBCR) && clock_on(GCC_USB_HS_SYSTEM_CBCR);
#if defined(TEST_USB_CLOCK)
	/* One clock on and the other not, the first time. */
	static bool partial;
	if (!partial) {
		partial = true;
		clocks = false;
		writel(readl(GCC_USB_HS_SYSTEM_CBCR) & ~1u, GCC_USB_HS_SYSTEM_CBCR);
	}
#endif
	if (!clocks) {
		usb_clocks_gate();
		usb_clocks_off = true;
		fail = "clocks";
		goto out;
	}
	usb_clocks_off = false;
	writel(readl(USB_USBCMD) & ~(USBCMD_STP | USBCMD_ASYNC_INTR), USB_USBCMD);
	writel(readl(USB_PORTSC) & ~PORTSC_PHCD, USB_PORTSC);
	bits_wait(USB_PORTSC, PORTSC_PHCD, 0);
	/* The vendor turns AHB2AHB bypass off before the asynchronous block
	 * reset. */
	writel(readl(USB_AHB_MODE) & ~AHB2AHB_BYPASS, USB_AHB_MODE);
	usb_clocks_gate();
	writel(1, GCC_USB_HS_BCR);
	delay_ms(1);
	writel(0, GCC_USB_HS_BCR);
	delay_us(1);
	if (!controller_init()) {
		fail = "controller reset";
		goto out;
	}
	delay_ms(100);
	phy_reset();
	for (unsigned i = 0; i < 4; i++)
		if (ulpi_write((uint8_t)(ULPI_PHY_OVERRIDE + i), board->usb_phy_init[i])) {
			fail = "phy overrides";
			goto out;
		}
	phy_reset();
#if defined(TEST_USB_FAIL)
	static bool injected;
	if (!injected) {
		injected = true;
		fail = "test";
		goto out;
	}
#endif
	fail = stack_up();
	if (fail)
		goto out;
	usb_down = false;
	log_printf("usb: up again in %lu ms\n", time_ms() - t0);
	return true;
out:
	usb_start_failed(fail, t0);
	return false;
}

/* The cable state from the PMIC: present keeps USB up (a wall charger
 * too, until it can be told apart from a host), absent for 2 s puts it
 * to rest. A failed bring-up is tried again after 5 s. */
#define GONE_MS		2000
#define RETRY_MS	5000
static uint32_t gone_t, retry_t;

void usb_cable(bool present, bool edge)
{
	uint32_t now = time_ms();
	if (!usb_down && dcd_stuck())
		usb_collapse("controller stuck");
	if (!present) {
		retry_t = 0;
		if (usb_down && (usb_clocks_off || rest_gave_up)) {
			gone_t = 0;
			return;
		}
		if (!gone_t)
			gone_t = now | 1;
		else if (now - gone_t >= GONE_MS) {
			gone_t = 0;
			usb_collapse("unplugged");
		}
		return;
	}
	gone_t = 0;
	if (edge)
		retry_t = 0;
	if (usb_down && (!retry_t || now - retry_t >= RETRY_MS))
		retry_t = usb_restore() ? 0 : now | 1;
}

/* When usb_cable wants to be called again: the end of the debounce or
 * the retry, and while USB is up a cable check every 2 s, as only a
 * cable plugged in raises an interrupt. Resting, nothing until then. */
uint32_t usb_next_ms(void)
{
	uint32_t now = time_ms(), next = UINT32_MAX;
	if (gone_t)
		next = ms_left(gone_t, GONE_MS, now);
	else if (!usb_down)
		next = GONE_MS;
	if (retry_t && usb_down) {
		uint32_t r = ms_left(retry_t, RETRY_MS, now);
		if (r < next)
			next = r;
	}
	return next;
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
	uint32_t t0 = time_ms();
	const char *fail = controller_init() ? NULL : "controller reset";
	irq_attach(IRQ_USB_HS, usb_irq);
	irq_mask(IRQ_USB_HS);
	static char banner[96];
	snprintf(banner, sizeof banner, "ro.product.name=%s;ro.product.model=asteroid-embedded;ro.product.device=%s", board->name,
		 board->name);
	adb_init(&transport, banner);
	if (!fail)
		fail = stack_up();
	if (fail)
		usb_start_failed(fail, t0);
}

/* Once per main loop pass, not from usb_poll: waits inside a pass pump
 * that as well. */
void usb_pass(void)
{
	if (cdc_blocked && !usb_down && tud_cdc_write_available())
		cdc_blocked = false;
}

void usb_poll(void)
{
	if (usb_down)
		return;
	tud_task();
	adb_poll();
	if (tud_cdc_connected())
		tud_cdc_write_flush();
	adb_queue_rx();
}

void usb_stats(uint32_t *rx, uint32_t *tx) { *rx = usb_rx_bytes; *tx = usb_tx_bytes; }

/* The stack has an event to handle, an adb transfer is in flight or
 * adb has more to send: the main loop should go round again before it
 * sleeps, as a completion that lands after its poll would otherwise
 * wait for the next tick. A host that takes nothing for STALL_MS is not
 * waited for like that any longer, or it would keep the watch from
 * sleeping for as long as it stays silent; the next transfer that
 * completes brings the waiting back. */
#define STALL_MS	2000
/* Calls further apart than this are not one wait. */
#define STALL_GAP_MS	500

bool usb_busy(void)
{
	static uint32_t since, last, seen;
	static bool stalled;
	uint32_t now = time_ms();
	bool adb = adb_tx_busy || adb_pending();
	bool busy = usb_connected() && (tud_task_event_ready() || adb);
	if (!adb || adb_xfers != seen || now - last > STALL_GAP_MS) {
		if (stalled && adb_xfers != seen)
			log_printf("usb: the host takes adb data again\n");
		if (adb_xfers != seen || !adb)
			stalled = false;
		since = now;
		seen = adb_xfers;
	}
	last = now;
	if (!busy)
		return false;
	if (stalled)
		return tud_task_event_ready();
	if (adb && now - since > STALL_MS) {
		log_printf("usb: the host takes no adb data (tx %d, pending %d), not waiting for it\n", adb_tx_busy,
			   adb_pending());
		stalled = true;
		return tud_task_event_ready();
	}
	return true;
}
