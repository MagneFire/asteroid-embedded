/* The APQ8026's USB block: the ChipIdea high-speed controller behind
 * the ULPI PHY the bootloader already brought up. The firmware
 * re-enables the clocks, resets the controller and keeps the PHY on
 * external VBUS sensing (as the bootloader runs it). */
#include "asteroid.h"
#include "usb/usb_port.h"
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

/* Both controller clocks are off, so no USB register may be touched. */
static bool usb_clocks_off;

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

bool usb_port_init(void) { return controller_init(); }

const char *usb_port_attach(void) { return phy_select() ? "phy select" : NULL; }

/* This comes from the port itself, as the controller only raises its
 * interrupts while the crystal runs: after an unplug the bus goes idle
 * and the port suspends, a newly attached host resets the port out of
 * suspend. */
bool usb_port_connected(void)
{
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

/* The APQ8009W's PHY is reset through its block reset in the GCC. */
static void phy_reset(void)
{
	if (chip->usb_phy_bcr) {
		writel(1, GCC(chip->usb_phy_bcr));
		delay_us(10);
		writel(0, GCC(chip->usb_phy_bcr));
		dsb();
		return;
	}
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

bool usb_port_rest(void)
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

bool usb_port_resting(void) { return usb_clocks_off; }

const char *usb_port_restore(void)
{
	if (!phy_rails(true))
		return "phy supplies";
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
		return "clocks";
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
	if (!controller_init())
		return "controller reset";
	delay_ms(100);
	phy_reset();
	for (unsigned i = 0; i < 4 && board->usb_phy_init[i]; i++)
		if (ulpi_write((uint8_t)(ULPI_PHY_OVERRIDE + i), board->usb_phy_init[i]))
			return "phy overrides";
	phy_reset();
	return NULL;
}
