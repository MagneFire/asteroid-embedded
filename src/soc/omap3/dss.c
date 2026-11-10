/* The display subsystem for a DSI command-mode panel on one data lane:
 * the DSI PLL from the 26 MHz crystal (416 MHz, a 104 MHz lane clock,
 * 138.7 MHz for the display controller and the DSI protocol engine),
 * the lane transmitters, the protocol engine with virtual channel 0,
 * and the display controller's graphics layer feeding the DSI video
 * port. A frame goes out as one run of DCS memory writes that the
 * display controller clocks through in stall mode. The values are the
 * vendor driver's for this panel; everything is set from scratch each
 * time the display comes up, as leaving it loses the lot. The interrupt
 * enables are set so that the status bits show, and nothing is routed
 * to the interrupt controller: all of it is polled. */
#include "asteroid.h"
#include "dss.h"

#define CM_DSS_FCLKEN		(CM_DSS + 0x00)
#define CM_DSS_ICLKEN		(CM_DSS + 0x10)
#define CM_DSS_IDLEST		(CM_DSS + 0x20)
#define CM_DSS_CLKSEL		(CM_DSS + 0x40)
#define CM_DSS_CLKSTCTRL	(CM_DSS + 0x48)
#define CM_CLKEN_PLL_REG	(CM_CCR + 0x00)
#define PWRDN_DSS1		BIT(29)
#define PM_DSS_PWSTCTRL		(PRM_BASE + 0xe00 + 0xe0)
#define PM_DSS_PWSTST		(PRM_BASE + 0xe00 + 0xe4)
#define FCLK_DSS1		BIT(0)
#define FCLK_DSS2		BIT(1)	/* the PLL's reference */
#define FCLK_TV			BIT(2)
/* 1728 MHz from DPLL4 down to 172.8 MHz. */
#define DSS1_DIVIDER		10

#define DSS_SYSCONFIG		(DSS_BASE + 0x10)
#define DSS_SYSSTATUS		(DSS_BASE + 0x14)
#define DSS_CONTROL		(DSS_BASE + 0x40)
#define DSS_SDI_CONTROL		(DSS_BASE + 0x44)
#define DSS_PLL_CONTROL		(DSS_BASE + 0x48)
#define DISPC_FROM_PLL		BIT(0)
#define DSI_FROM_PLL		BIT(1)

#define DISPC_SYSCONFIG		(DISPC_BASE + 0x10)
#define DISPC_IRQSTATUS		(DISPC_BASE + 0x18)
#define DISPC_IRQENABLE		(DISPC_BASE + 0x1c)
#define DISPC_CONTROL		(DISPC_BASE + 0x40)
#define DISPC_CONFIG		(DISPC_BASE + 0x44)
#define DISPC_DEFAULT_COLOR	(DISPC_BASE + 0x4c)
#define DISPC_TRANS_COLOR	(DISPC_BASE + 0x54)
#define DISPC_TIMING_H		(DISPC_BASE + 0x64)
#define DISPC_TIMING_V		(DISPC_BASE + 0x68)
#define DISPC_POL_FREQ		(DISPC_BASE + 0x6c)
#define DISPC_DIVISOR		(DISPC_BASE + 0x70)
#define DISPC_GLOBAL_ALPHA	(DISPC_BASE + 0x74)
#define DISPC_SIZE_LCD		(DISPC_BASE + 0x7c)
#define DISPC_GFX_BA0		(DISPC_BASE + 0x80)
#define DISPC_GFX_BA1		(DISPC_BASE + 0x84)
#define DISPC_GFX_POSITION	(DISPC_BASE + 0x88)
#define DISPC_GFX_SIZE		(DISPC_BASE + 0x8c)
#define DISPC_GFX_ATTRIBUTES	(DISPC_BASE + 0xa0)
#define DISPC_GFX_FIFO		(DISPC_BASE + 0xa4)
#define DISPC_GFX_ROW_INC	(DISPC_BASE + 0xac)
#define DISPC_GFX_PIXEL_INC	(DISPC_BASE + 0xb0)
#define DISPC_FRAMEDONE		BIT(0)
#define DISPC_LCDENABLE		BIT(0)
#define DISPC_PCKFREE		BIT(27)
/* Smart idle and standby, wake-up, auto idle; no idle while a frame
 * is on its way. */
#define DISPC_SYSCONFIG_IDLE	0x2015u
#define DISPC_SYSCONFIG_BUSY	0x200du
/* TFT, 24 data lines, stall mode, the two bypass outputs. */
#define DISPC_CONTROL_STALL	0x00018b08u
/* FIFO handcheck, functional clock gated, frame data loaded per frame. */
#define DISPC_CONFIG_STALL	0x00010204u
/* Logic clock 1:1, pixel clock a 15th. */
#define DISPC_DIVISOR_VALUE	0x0001000fu
/* XRGB8888 in 32 bits, bursts of 8, on. */
#define GFX_ATTRIBUTES_XRGB	0x91u
#define GFX_FIFO_THRESHOLDS	0x03c00380u

#define DSI_SYSCONFIG		(DSI_BASE + 0x010)
#define DSI_SYSSTATUS		(DSI_BASE + 0x014)
#define DSI_IRQSTATUS		(DSI_BASE + 0x018)
#define DSI_IRQENABLE		(DSI_BASE + 0x01c)
#define DSI_CTRL		(DSI_BASE + 0x040)
#define DSI_CIO_CFG1		(DSI_BASE + 0x048)
#define DSI_CIO_IRQSTATUS	(DSI_BASE + 0x04c)
#define DSI_CIO_IRQENABLE	(DSI_BASE + 0x050)
#define DSI_CLK_CTRL		(DSI_BASE + 0x054)
#define DSI_TIMING1		(DSI_BASE + 0x058)
#define DSI_TIMING2		(DSI_BASE + 0x05c)
#define DSI_CLK_TIMING		(DSI_BASE + 0x06c)
#define DSI_TX_FIFO_SIZE	(DSI_BASE + 0x070)
#define DSI_RX_FIFO_SIZE	(DSI_BASE + 0x074)
#define DSI_CIO_CFG2		(DSI_BASE + 0x078)
#define DSI_VM_TIMING7		(DSI_BASE + 0x090)
#define DSI_VC_CTRL(n)		(DSI_BASE + 0x100 + 0x20 * (n))
#define DSI_VC_TE		(DSI_BASE + 0x104)
#define DSI_VC_LONG_HEADER	(DSI_BASE + 0x108)
#define DSI_VC_LONG_PAYLOAD	(DSI_BASE + 0x10c)
#define DSI_VC_SHORT_HEADER	(DSI_BASE + 0x110)
#define DSI_VC_IRQSTATUS	(DSI_BASE + 0x118)
#define DSI_VC_IRQENABLE(n)	(DSI_BASE + 0x11c + 0x20 * (n))
#define DSI_PHY_CFG0		(DSI_BASE + 0x200)
#define DSI_PHY_CFG1		(DSI_BASE + 0x204)
#define DSI_PHY_CFG2		(DSI_BASE + 0x208)
#define DSI_PHY_CFG5		(DSI_BASE + 0x214)
#define DSI_PHY_CFG10		(DSI_BASE + 0x228)
#define DSI_PLL_CONTROL		(DSI_BASE + 0x300)
#define DSI_PLL_STATUS		(DSI_BASE + 0x304)
#define DSI_PLL_GO		(DSI_BASE + 0x308)
#define DSI_PLL_CONFIG1		(DSI_BASE + 0x30c)
#define DSI_PLL_CONFIG2		(DSI_BASE + 0x310)

#define DSI_ERRORS		0x0014c000u
#define CTRL_IF_EN		BIT(0)
#define CIO_PWR_CMD(n)		((uint32_t)(n) << 27)
#define CIO_PWR_STATE(v)	((v) >> 25 & 3)
#define CIO_RESET_DONE		BIT(29)
#define CIO_ULPS_ALL		BIT(30)
#define CLK_CIO_GATE		BIT(14)
#define CLK_LP_ENABLE		BIT(20)
#define CLK_PLL_PWR(n)		((uint32_t)(n) << 30)
#define CLK_PLL_STATE(v)	((v) >> 28 & 3)
#define TIMING1_FORCE_STOP	BIT(15)
#define VC_EN			BIT(0)
#define VC_SOURCE_VP		BIT(1)
#define VC_TX_NOT_EMPTY		BIT(5)
#define VC_BTA_EN		BIT(6)
#define VC_HIGH_SPEED		BIT(9)
#define VC_BUSY			BIT(15)
#define VC_TX_FULL		BIT(16)
#define VC_RX_NOT_EMPTY		BIT(20)
#define VC_IRQ_BTA		BIT(5)
#define TE_START		BIT(31)
#define TE_EN			BIT(30)
#define PLL_RESET_DONE		BIT(0)
#define PLL_LOCK		BIT(1)
#define PLL_DISPC_ACTIVE	BIT(7)
#define PLL_DSI_ACTIVE		BIT(8)
#define PHY_SCP_RESET_DONE	BIT(30)
#define PHY_ESC_RESET_DONE	(BIT(24) | BIT(25))
/* The clock lane and data lane 1 held high by hand, to leave ULPS. */
#define PHY_OVERRIDE_LANES	(0x0au << 17)
#define PHY_OVERRIDE_EN		BIT(27)

#define DT_DCS_SHORT0		0x05
#define DT_DCS_LONG		0x39
#define DT_DCS_READ		0x06
#define DT_GENERIC_SHORT0	0x03
#define DT_GENERIC_LONG		0x29
#define DT_GENERIC_READ2	0x24
#define DT_SET_MAX_RETURN	0x37
#define DT_ACK_ERROR		0x02

/* The line buffer holds 1023 pixels, and a packet whole lines. */
#define LINE_BUFFER_BYTES	(1023u * 3)

static bool high_speed, source_vp, up;
static uint32_t frame_w, frame_h;

static bool wait_bits(uint32_t addr, uint32_t mask, uint32_t want, uint32_t us)
{
	uint32_t t0 = time_us();
	while ((readl(addr) & mask) != want)
		if (time_us() - t0 > us)
			return false;
	return true;
}

static void set_bits(uint32_t addr, uint32_t mask, uint32_t value)
{
	writel((readl(addr) & ~mask) | value, addr);
}

static bool step(bool ok, const char *what)
{
	if (!ok)
		log_printf("dss: %s, dsi irq %08lx cio %08lx pll %08lx\n", what, readl(DSI_IRQSTATUS), readl(DSI_CIO_IRQSTATUS),
			   readl(DSI_PLL_STATUS));
	return ok;
}

static bool module_on(void)
{
	set_bits(CM_CLKEN_PLL_REG, PWRDN_DSS1, 0);
	set_bits(CM_DSS_CLKSEL, 0x3f, DSS1_DIVIDER);
	set_bits(PM_DSS_PWSTCTRL, 3, 3);
	/* The domain is held awake for as long as the modules are in use:
	 * left to itself it stops the clock of one in reset, and reading
	 * that one faults. */
	writel(2, CM_DSS_CLKSTCTRL);
	writel(FCLK_DSS1 | FCLK_DSS2 | FCLK_TV, CM_DSS_FCLKEN);
	writel(1, CM_DSS_ICLKEN);
	if (!wait_bits(PM_DSS_PWSTST, 3, 3, 100000) || !wait_bits(CM_DSS_IDLEST, BIT(1), 0, 100000))
		return false;
	writel(0, DSS_CONTROL);
	/* A frame the bootloader left going is let out first. */
	if (readl(DISPC_CONTROL) & 1) {
		writel(DISPC_FRAMEDONE, DISPC_IRQSTATUS);
		set_bits(DISPC_CONTROL, 3, 0);
		wait_bits(DISPC_IRQSTATUS, DISPC_FRAMEDONE, DISPC_FRAMEDONE, 100000);
	}
	set_bits(DISPC_CONTROL, 3, 0);
	writel(0, DSS_SDI_CONTROL);
	writel(0, DSS_PLL_CONTROL);
	writel(BIT(1), DSS_SYSCONFIG);
	bool ok = wait_bits(DSS_SYSSTATUS, 1, 1, 100000);
	/* Out of reset the controller asks to idle at once. */
	writel(DISPC_SYSCONFIG_IDLE, DISPC_SYSCONFIG);
	writel(FCLK_DSS1 | FCLK_DSS2, CM_DSS_FCLKEN);
	return ok;
}

static void module_off(void)
{
	writel(0, CM_DSS_FCLKEN);
	writel(0, CM_DSS_ICLKEN);
	writel(3, CM_DSS_CLKSTCTRL);
}

/* 26 MHz / 13 * 104 = 416 MHz, a third of it for each of the two
 * dividers. The PLL only leaves its reset with the pixel clock forced
 * on for a moment, and of its power states the one for everything
 * does not work: the next one down does. */
static bool pll_on(void)
{
	set_bits(DSI_CLK_CTRL, CLK_CIO_GATE, CLK_CIO_GATE);
	set_bits(DISPC_CONTROL, DISPC_PCKFREE, DISPC_PCKFREE);
	bool ok = wait_bits(DSI_PLL_STATUS, PLL_RESET_DONE, PLL_RESET_DONE, 500000);
	set_bits(DISPC_CONTROL, DISPC_PCKFREE, 0);
	if (!step(ok, "pll stays in reset"))
		return false;
	set_bits(DSI_CLK_CTRL, CLK_PLL_PWR(3), CLK_PLL_PWR(2));
	uint32_t t0 = time_us();
	while (CLK_PLL_STATE(readl(DSI_CLK_CTRL)) != 2)
		if (time_us() - t0 > 10000)
			return step(false, "pll without power");
	set_bits(DSI_PLL_CONTROL, 1, 0);
	writel(0x01106819, DSI_PLL_CONFIG1);
	writel(0x0010200e, DSI_PLL_CONFIG2);
	writel(1, DSI_PLL_GO);
	if (!step(wait_bits(DSI_PLL_GO, 1, 0, 500000), "pll go not taken") ||
	    !step(wait_bits(DSI_PLL_STATUS, PLL_LOCK, PLL_LOCK, 500000), "pll not locked"))
		return false;
	writel(0x0005600e, DSI_PLL_CONFIG2);
	if (!step(wait_bits(DSI_PLL_STATUS, PLL_DSI_ACTIVE, PLL_DSI_ACTIVE, 500000), "no dsi clock from the pll"))
		return false;
	set_bits(DSS_CONTROL, DSI_FROM_PLL, DSI_FROM_PLL);
	return true;
}

static bool cio_power(unsigned state)
{
	set_bits(DSI_CIO_CFG1, CIO_PWR_CMD(3), CIO_PWR_CMD(state));
	uint32_t t0 = time_us();
	while (CIO_PWR_STATE(readl(DSI_CIO_CFG1)) != state)
		if (time_us() - t0 > 10000)
			return false;
	return true;
}

static bool if_enable(bool on)
{
	set_bits(DSI_CTRL, CTRL_IF_EN, on ? CTRL_IF_EN : 0);
	return wait_bits(DSI_CTRL, CTRL_IF_EN, on ? CTRL_IF_EN : 0, 100000);
}

static bool vc_enable(unsigned n, bool on)
{
	set_bits(DSI_VC_CTRL(n), VC_EN, on ? VC_EN : 0);
	return wait_bits(DSI_VC_CTRL(n), VC_EN, on ? VC_EN : 0, 100000);
}

static bool force_stop(void)
{
	set_bits(DSI_TIMING1, TIMING1_FORCE_STOP, TIMING1_FORCE_STOP);
	return wait_bits(DSI_TIMING1, TIMING1_FORCE_STOP, 0, 100000);
}

/* The lane transmitters: the clock on the first pair and data on the
 * second. Lanes left in ULPS are brought out of it by driving them
 * high by hand while the transmitters start. */
static bool cio_on(bool from_ulps)
{
	(void)readl(DSI_PHY_CFG5);
	if (!step(wait_bits(DSI_PHY_CFG5, PHY_SCP_RESET_DONE, PHY_SCP_RESET_DONE, 500000), "phy registers stay in reset"))
		return false;
	set_bits(DSI_CIO_CFG1, 0xfff, 0x021);
	set_bits(DSI_TIMING1, 0xffff, 0xffff);
	if (from_ulps) {
		set_bits(DSI_PHY_CFG10, 0x3fu << 17, PHY_OVERRIDE_LANES);
		set_bits(DSI_PHY_CFG10, PHY_OVERRIDE_EN, PHY_OVERRIDE_EN);
	}
	bool ok = step(cio_power(1), "lanes without power") &&
		  step(wait_bits(DSI_CIO_CFG1, CIO_RESET_DONE, CIO_RESET_DONE, 500000), "lanes stay in reset");
	if (ok) {
		if_enable(true);
		if_enable(false);
		set_bits(DSI_CLK_CTRL, CLK_LP_ENABLE, CLK_LP_ENABLE);
		ok = step(wait_bits(DSI_PHY_CFG5, PHY_ESC_RESET_DONE, PHY_ESC_RESET_DONE, 100000), "no escape clock on the lanes");
	}
	if (from_ulps) {
		delay_ms(1);
		set_bits(DSI_PHY_CFG10, PHY_OVERRIDE_EN, 0);
		set_bits(DSI_PHY_CFG10, 0x3fu << 17, 0);
	}
	if (!ok)
		return false;
	set_bits(DSI_TIMING1, TIMING1_FORCE_STOP, 0);
	/* Lane timings for the 104 MHz clock. */
	writel(0x0a150c10, DSI_PHY_CFG0);
	set_bits(DSI_PHY_CFG1, 0x1fffff, 0x03091c);
	set_bits(DSI_PHY_CFG2, 0xff, 0x07);
	return true;
}

static bool protocol_on(void)
{
	writel(0x00001010, DSI_CLK_TIMING);
	writel(0x000a000c, DSI_VM_TIMING7);
	/* The low power clock an eighth of the escape clock: 8.67 MHz. */
	set_bits(DSI_CLK_CTRL, 0x1fff | BIT(21), 8 | BIT(21));
	writel(0x13121110, DSI_TX_FIFO_SIZE);
	writel(0x13121110, DSI_RX_FIFO_SIZE);
	writel(0xffff9000, DSI_TIMING1);
	writel(0xffff7fff, DSI_TIMING2);
	/* Checksums and ECC, 24 bits a pixel, two lines of buffer, the
	 * memory write command put in front of the pixels. */
	writel(0x0108609e, DSI_CTRL);
	for (unsigned n = 0; n < 4; n++)
		writel(0x20800180, DSI_VC_CTRL(n));
	high_speed = source_vp = false;
	for (unsigned n = 0; n < 4; n++)
		if (!vc_enable(n, true))
			return step(false, "channel not enabled");
	return step(if_enable(true), "interface not enabled") && step(force_stop(), "lanes not stopped");
}

static void dispc_setup(void)
{
	writel(DISPC_SYSCONFIG_IDLE, DISPC_SYSCONFIG);
	writel(DISPC_CONTROL_STALL, DISPC_CONTROL);
	writel(DISPC_CONFIG_STALL, DISPC_CONFIG);
	writel(DISPC_DIVISOR_VALUE, DISPC_DIVISOR);
	writel(0, DISPC_TIMING_H);
	writel(0, DISPC_TIMING_V);
	writel(0, DISPC_POL_FREQ);
	uint32_t size = (frame_h - 1) << 16 | (frame_w - 1);
	writel(size, DISPC_SIZE_LCD);
	writel(0, DISPC_DEFAULT_COLOR);
	writel(0, DISPC_TRANS_COLOR);
	writel(0, DISPC_GFX_POSITION);
	writel(size, DISPC_GFX_SIZE);
	writel(GFX_ATTRIBUTES_XRGB, DISPC_GFX_ATTRIBUTES);
	writel(GFX_FIFO_THRESHOLDS, DISPC_GFX_FIFO);
	writel(1, DISPC_GFX_ROW_INC);
	writel(1, DISPC_GFX_PIXEL_INC);
	set_bits(DISPC_GLOBAL_ALPHA, 0xff, 0xff);
	writel(~0u, DISPC_IRQSTATUS);
	writel(DISPC_FRAMEDONE, DISPC_IRQENABLE);
}

bool dss_on(uint32_t width, uint32_t height, bool from_ulps)
{
	frame_w = width;
	frame_h = height;
	up = false;
	if (!module_on()) {
		log_printf("dss: no power or clock, pwstst %08lx idlest %08lx\n", readl(PM_DSS_PWSTST), readl(CM_DSS_IDLEST));
		return false;
	}
	set_bits(DISPC_CONFIG, 0x206, 0x204);
	writel(BIT(1), DSI_SYSCONFIG);
	if (!step(wait_bits(DSI_SYSSTATUS, 1, 1, 100000), "dsi stays in reset"))
		return false;
	writel(0x0015c000, DSI_IRQENABLE);
	for (unsigned n = 0; n < 4; n++)
		writel(0xdb | VC_IRQ_BTA, DSI_VC_IRQENABLE(n));
	writel(0x3ff07fff | CIO_ULPS_ALL, DSI_CIO_IRQENABLE);
	if (!pll_on() || !cio_on(from_ulps) || !protocol_on())
		return false;
	if (!step(wait_bits(DSI_PLL_STATUS, PLL_DISPC_ACTIVE, PLL_DISPC_ACTIVE, 500000), "no display clock from the pll"))
		return false;
	set_bits(DSS_CONTROL, DISPC_FROM_PLL, DISPC_FROM_PLL);
	dispc_setup();
	up = true;
	return true;
}

/* The lanes to their ultra low power state, where the panel's side
 * stays until dss_on brings them back. */
static void ulps_enter(void)
{
	force_stop();
	for (unsigned n = 0; n < 4; n++)
		vc_enable(n, false);
	if (readl(DSI_CIO_CFG2) & (BIT(16) | BIT(17))) {
		log_printf("dss: lanes busy, not entering ULPS\n");
		return;
	}
	writel(CIO_ULPS_ALL, DSI_CIO_IRQSTATUS);
	set_bits(DSI_CIO_CFG2, 0x1fu << 5, 0x03u << 5);
	(void)readl(DSI_CIO_CFG2);
	if (!wait_bits(DSI_CIO_IRQSTATUS, CIO_ULPS_ALL, CIO_ULPS_ALL, 1000000))
		log_printf("dss: lanes did not reach ULPS, cio %08lx\n", readl(DSI_CIO_IRQSTATUS));
	set_bits(DSI_CIO_CFG2, 0x1fu << 5, 0);
	cio_power(2);
	if_enable(false);
}

void dss_off(bool ulps)
{
	if (!(readl(CM_DSS_ICLKEN) & 1))
		return;
	set_bits(DSS_CONTROL, DISPC_FROM_PLL, 0);
	if (ulps && up)
		ulps_enter();
	if_enable(false);
	set_bits(DSS_CONTROL, DSI_FROM_PLL, 0);
	cio_power(0);
	set_bits(DSI_CLK_CTRL, CLK_CIO_GATE, 0);
	set_bits(DSI_CLK_CTRL, CLK_PLL_PWR(3), 0);
	module_off();
	up = false;
}

bool dss_up(void) { return up; }

/* Everything sent has left, on the way commands go or the way pixels
 * go. */
static bool channel_idle(void)
{
	if (source_vp)
		return wait_bits(DSI_VC_TE, TE_START | TE_EN, 0, 250000);
	return wait_bits(DSI_VC_CTRL(0), VC_TX_NOT_EMPTY, 0, 20000);
}

static bool source(bool vp)
{
	if (vp == source_vp)
		return true;
	if (!channel_idle() || !vc_enable(0, false) || !wait_bits(DSI_VC_CTRL(0), VC_BUSY, 0, 100000))
		return false;
	set_bits(DSI_VC_CTRL(0), VC_SOURCE_VP, vp ? VC_SOURCE_VP : 0);
	source_vp = vp;
	return vc_enable(0, true);
}

void dsi_high_speed(bool on)
{
	if (!up || on == high_speed)
		return;
	channel_idle();
	vc_enable(0, false);
	if_enable(false);
	set_bits(DSI_VC_CTRL(0), VC_HIGH_SPEED, on ? VC_HIGH_SPEED : 0);
	vc_enable(0, true);
	if_enable(true);
	force_stop();
	high_speed = on;
}

/* Up to two bytes go as a short packet, whose type says how many: a
 * DCS one counts its command among them, a generic one has none. */
static int send(bool generic, const uint8_t *d, unsigned n)
{
	if (!up || !source(false) || (!generic && !n))
		return -1;
	if (n <= 2) {
		if (readl(DSI_VC_CTRL(0)) & VC_TX_FULL)
			return -2;
		uint32_t type = generic ? DT_GENERIC_SHORT0 + 0x10 * n : DT_DCS_SHORT0 + 0x10 * (n - 1);
		writel(type | (n ? (uint32_t)d[0] << 8 : 0) | (n > 1 ? (uint32_t)d[1] << 16 : 0), DSI_VC_SHORT_HEADER);
		return 0;
	}
	if (n + 4 > 128)
		return -3;
	if (!wait_bits(DSI_VC_CTRL(0), VC_TX_NOT_EMPTY, 0, 20000))
		return -2;
	writel((generic ? DT_GENERIC_LONG : DT_DCS_LONG) | n << 8, DSI_VC_LONG_HEADER);
	for (unsigned i = 0; i < n; i += 4) {
		uint32_t w = 0;
		for (unsigned k = 0; k < 4 && i + k < n; k++)
			w |= (uint32_t)d[i + k] << (8 * k);
		writel(w, DSI_VC_LONG_PAYLOAD);
	}
	return 0;
}

static void rx_flush(void)
{
	for (unsigned i = 0; i < 64 && (readl(DSI_VC_CTRL(0)) & VC_RX_NOT_EMPTY); i++)
		(void)readl(DSI_VC_SHORT_HEADER);
}

/* The bus handed to the panel's side and back: by then it has taken
 * what was sent, and what it had to say is in the receive FIFO. */
static int turnaround(void)
{
	if (!channel_idle())
		return -4;
	writel(~0u, DSI_VC_IRQSTATUS);
	writel(~0u, DSI_IRQSTATUS);
	set_bits(DSI_VC_CTRL(0), VC_BTA_EN, VC_BTA_EN);
	(void)readl(DSI_VC_CTRL(0));
	uint32_t t0 = time_us();
	while (!(readl(DSI_VC_IRQSTATUS) & VC_IRQ_BTA)) {
		if (readl(DSI_IRQSTATUS) & DSI_ERRORS)
			return -5;
		if (time_us() - t0 > 50000)
			return -6;
	}
	return 0;
}

int dsi_sync(void)
{
	if (!up || !source(false))
		return -1;
	if (readl(DSI_VC_CTRL(0)) & VC_RX_NOT_EMPTY)
		rx_flush();
	int r = turnaround();
	if (!r && (readl(DSI_VC_CTRL(0)) & VC_RX_NOT_EMPTY)) {
		uint32_t hdr = readl(DSI_VC_SHORT_HEADER);
		rx_flush();
		if ((hdr & 0x3f) == DT_ACK_ERROR)
			r = -7;
	}
	return r;
}

int dsi_write(bool generic, const uint8_t *d, unsigned n, bool sync)
{
	int r = send(generic, d, n);
	return r || !sync ? r : dsi_sync();
}

/* An answer of up to n bytes to a DCS read of one command, or to a
 * generic read with two parameters. */
int dsi_read(bool generic, const uint8_t *req, uint8_t *buf, unsigned n)
{
	if (!up || !source(false))
		return -1;
	rx_flush();
	if (readl(DSI_VC_CTRL(0)) & VC_TX_FULL)
		return -2;
	writel(DT_SET_MAX_RETURN | n << 8, DSI_VC_SHORT_HEADER);
	writel(generic ? DT_GENERIC_READ2 | (uint32_t)req[0] << 8 | (uint32_t)req[1] << 16 : DT_DCS_READ | (uint32_t)req[0] << 8,
	       DSI_VC_SHORT_HEADER);
	int r = turnaround();
	if (r)
		return r;
	if (!(readl(DSI_VC_CTRL(0)) & VC_RX_NOT_EMPTY))
		return -8;
	uint32_t hdr = readl(DSI_VC_SHORT_HEADER), type = hdr & 0x3f;
	unsigned got = 0;
	if (type == 0x11 || type == 0x21 || type == 0x12 || type == 0x22) {
		unsigned len = type & 3;
		for (; got < len && got < n; got++)
			buf[got] = (uint8_t)(hdr >> (8 + 8 * got));
	} else if (type == 0x1a || type == 0x1c) {
		unsigned len = hdr >> 8 & 0xffff;
		for (unsigned i = 0; i < len + 2; i += 4) {
			uint32_t w = readl(DSI_VC_SHORT_HEADER);
			for (unsigned k = 0; k < 4 && i + k < len; k++)
				if (got < n)
					buf[got++] = (uint8_t)(w >> (8 * k));
		}
	} else {
		rx_flush();
		return type == DT_ACK_ERROR ? -7 : -9;
	}
	rx_flush();
	writel(DT_SET_MAX_RETURN | 1u << 8, DSI_VC_SHORT_HEADER);
	return (int)got;
}

/* The frame at buf to the panel's memory, the window set by the caller.
 * With te the transfer waits for the panel's tearing signal, which it
 * sends once it is handed the bus; without, it starts at once. */
bool dss_frame_start(const uint32_t *buf, bool te)
{
	if (!up || !source(true))
		return false;
	uint32_t line = frame_w * 3, packet = LINE_BUFFER_BYTES / line * line, total = line * frame_h;
	uint32_t size = total / packet * (packet + 1) + (total % packet ? total % packet + 1 : 0);
	writel(size, DSI_VC_TE);
	writel(DT_DCS_LONG | (packet + 1) << 8, DSI_VC_LONG_HEADER);
	writel(size | (te ? TE_EN : TE_START), DSI_VC_TE);
	writel(DISPC_SYSCONFIG_BUSY, DISPC_SYSCONFIG);
	writel(DISPC_FRAMEDONE, DISPC_IRQSTATUS);
	writel((uint32_t)(uintptr_t)buf, DISPC_GFX_BA0);
	writel((uint32_t)(uintptr_t)buf, DISPC_GFX_BA1);
	writel(DISPC_CONTROL_STALL | DISPC_LCDENABLE, DISPC_CONTROL);
	if (te)
		set_bits(DSI_VC_CTRL(0), VC_BTA_EN, VC_BTA_EN);
	return true;
}

/* The display controller has passed the whole frame on and the DSI
 * side has sent it. */
bool dss_frame_done(void)
{
	if (!(readl(DISPC_IRQSTATUS) & DISPC_FRAMEDONE) || (readl(DSI_VC_TE) & (TE_START | TE_EN)))
		return false;
	writel(DISPC_FRAMEDONE, DISPC_IRQSTATUS);
	writel(DISPC_SYSCONFIG_IDLE, DISPC_SYSCONFIG);
	return true;
}

/* A frame that never finished: the transfer is taken back. */
void dss_frame_abort(void)
{
	log_printf("dss: frame not done, dispc irq %08lx te %08lx dsi irq %08lx vc %08lx\n", readl(DISPC_IRQSTATUS), readl(DSI_VC_TE),
		   readl(DSI_IRQSTATUS), readl(DSI_VC_IRQSTATUS));
	set_bits(DISPC_CONTROL, DISPC_LCDENABLE, 0);
	vc_enable(0, false);
	writel(0, DSI_VC_TE);
	vc_enable(0, true);
	writel(~0u, DISPC_IRQSTATUS);
	writel(~0u, DSI_IRQSTATUS);
	writel(DISPC_SYSCONFIG_IDLE, DISPC_SYSCONFIG);
}
