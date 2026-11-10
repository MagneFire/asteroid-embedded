/* The OMAP3's USB block: the Mentor controller behind an external ULPI
 * PHY. The PHY has no registers anyone uses: three lines power it, it
 * runs from the 26 MHz clock it shares with the panel's bridge, and a
 * fourth line is high while VBUS is there. The controller itself is
 * clocked by the PHY, so the PHY comes up first. Before the device
 * attaches the PHY is switched off and on once more, as the vendor's
 * driver does to make a second connection work. Unplugged, the PHY is
 * off and the controller's clock gated. */
#include "asteroid.h"
#include "tusb.h"
#include "usb/usb_port.h"

#define OTG_SYSCONFIG		(MUSB_BASE + 0x404)
#define OTG_SYSSTATUS		(MUSB_BASE + 0x408)
#define OTG_INTERFSEL		(MUSB_BASE + 0x40c)
#define SYSCONFIG_RESET		BIT(1)
/* No idle, standby forced: any other standby mode keeps the chip's
 * core from idling. */
#define SYSCONFIG_RUN		(1u << 3)
#define SYSCONFIG_REST		0
#define INTERFSEL_ULPI_12PIN	1
#define MUSB_POWER		(MUSB_BASE + 0x01)
#define MUSB_INTRTXE		(MUSB_BASE + 0x06)
#define MUSB_INTRRXE		(MUSB_BASE + 0x08)
#define MUSB_INTRUSBE		(MUSB_BASE + 0x0b)
#define MUSB_DEVCTL		(MUSB_BASE + 0x60)
#define MUSB_HWVERS		(MUSB_BASE + 0x6c)
#define POWER_SOFTCONN		BIT(6)
#define DEVCTL_VBUS_VALID	(3u << 3)
#define PHY_SETTLE_MS		10

#define PHY	(board->usb_phy)

static bool resting = true, phy_on;
static volatile bool plugged;

static inline uint8_t rb(uint32_t a) { return *(volatile uint8_t *)(uintptr_t)a; }
static inline void wb(uint8_t v, uint32_t a) { *(volatile uint8_t *)(uintptr_t)a = v; }
static inline void ww(uint16_t v, uint32_t a) { *(volatile uint16_t *)(uintptr_t)a = v; }

bool vbus_present(void)
{
	return gpio_get(PHY.vbus);
}

static void vbus_edge(unsigned pin) { plugged = true; }

/* A cable going in, which also wakes the watch through the pad. */
void vbus_irq_init(void)
{
	gpio_config(PHY.vbus, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	gpio_irq_config(PHY.vbus, GPIO_IRQ_EDGE_RISING, vbus_edge);
	gpio_irq_enable(PHY.vbus, true);
}

bool vbus_irq(void)
{
	if (!plugged)
		return false;
	plugged = false;
	return true;
}

static void phy_power(bool on)
{
	if (on == phy_on)
		return;
	if (on)
		periph_clock(true);
	gpio_set(PHY.resetn, on);
	gpio_set(PHY.cs, on);
	gpio_set(PHY.csn, !on);
	gpio_config(PHY.resetn, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_config(PHY.cs, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_config(PHY.csn, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	if (!on)
		periph_clock(false);
	phy_on = on;
	if (on)
		delay_ms(PHY_SETTLE_MS);
}

static void core_quiet(void)
{
	wb(rb(MUSB_POWER) & (uint8_t)~POWER_SOFTCONN, MUSB_POWER);
	ww(0, MUSB_INTRTXE);
	ww(0, MUSB_INTRRXE);
	wb(0, MUSB_INTRUSBE);
	wb(0, MUSB_DEVCTL);
}

const char *usb_port_restore(void)
{
	clock_enable(CM_CORE, CORE_HSOTGUSB, true);
	phy_power(true);
	writel(SYSCONFIG_RESET, OTG_SYSCONFIG);
	uint32_t t0 = time_ms();
	while (!(readl(OTG_SYSSTATUS) & 1))
		if (time_ms() - t0 > 100)
			return "controller reset";
	writel(SYSCONFIG_RUN, OTG_SYSCONFIG);
	writel(INTERFSEL_ULPI_12PIN, OTG_INTERFSEL);
	core_quiet();
	/* The PHY says what VBUS is as it starts and when it changes, and
	 * the reset made the controller forget: the PHY starts once more
	 * for the controller to hear it. */
	phy_power(false);
	delay_ms(PHY_SETTLE_MS);
	phy_power(true);
	uint32_t t1 = time_ms();
	while (vbus_present() && (rb(MUSB_DEVCTL) & DEVCTL_VBUS_VALID) != DEVCTL_VBUS_VALID && time_ms() - t1 < 100)
		;
	log_printf("usb: devctl %02x after %lu ms\n", rb(MUSB_DEVCTL), time_ms() - t1);
	resting = false;
	return NULL;
}

/* The bootloader's fastboot may still be attached: the host has to see
 * it go before the firmware's device comes. */
bool usb_port_init(void)
{
	clock_enable(CM_CORE, CORE_HSOTGUSB, true);
	if (phy_on || gpio_get(PHY.resetn))
		wb(rb(MUSB_POWER) & (uint8_t)~POWER_SOFTCONN, MUSB_POWER);
	const char *fail = usb_port_restore();
	log_printf("usb: controller version %04x, vbus %u%s%s\n", *(volatile uint16_t *)(uintptr_t)MUSB_HWVERS, vbus_present(),
		   fail ? ", " : "", fail ? fail : "");
	return !fail;
}

/* The controller has no clock while the PHY is off, so its interrupt
 * stays out meanwhile. */
const char *usb_port_attach(void)
{
	irq_mask(USB_IRQ);
	phy_power(false);
	delay_ms(PHY_SETTLE_MS);
	phy_power(true);
	irq_unmask(USB_IRQ);
	return NULL;
}

bool usb_port_rest(void)
{
	if (resting)
		return true;
	core_quiet();
	phy_power(false);
	writel(SYSCONFIG_REST, OTG_SYSCONFIG);
	clock_enable(CM_CORE, CORE_HSOTGUSB, false);
	resting = true;
	return true;
}

bool usb_port_resting(void) { return resting; }

/* The controller has no bit for it: a host that has spoken to the
 * device and not suspended the bus since. */
bool usb_port_connected(void)
{
	return vbus_present() && tud_connected() && !tud_suspended();
}
