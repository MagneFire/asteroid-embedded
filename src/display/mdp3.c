/* The APQ8009W's MDP3: the bootloader leaves DMA_P sending a buffer of
 * its own to the DSI command-mode panel. DMA_P is re-pointed at the
 * firmware's buffers in XRGB8888; there is no mixer or pipe, so a frame
 * is the buffer's address and a start. The tear check waits for the
 * panel's TE line as on the MDP5, and DMA_P_DONE ends a frame. */
#include "asteroid.h"
#include "display.h"
#include "mdp3.h"

#define MDP3(off)		(chip->mdp3 + (off))
#define INTR_ENABLE		MDP3(0x0020)
#define INTR_STATUS		MDP3(0x0024)
#define INTR_CLEAR		MDP3(0x0028)
#define INTR_DMA_P_DONE		BIT(14)
#define DMA_P_START		MDP3(0x0044)
#define HW_VERSION		MDP3(0x0070)
#define SYNC_THRESH_0		MDP3(0x0200)
#define TEAR_CHECK_EN		MDP3(0x020c)
#define PRIMARY_START_POS	MDP3(0x0210)
#define PRIMARY_RD_PTR_IRQ	MDP3(0x021c)
#define SYNC_CONFIG_0		MDP3(0x0300)
#define SYNC_VSYNC_COUNTER_EN	BIT(19)
#define SYNC_VSYNC_IN_EN	BIT(20)
#define VSYNC_SEL		MDP3(0x0324)
#define PRIMARY_VSYNC_INIT_VAL	MDP3(0x0328)
#define AUTOREFRESH_CONFIG_P	MDP3(0x034c)
#define DMA_P_CONFIG		MDP3(0x90000)
#define DMA_P_OUT_DSI_CMD	(1u << 19)
#define DMA_P_OUT_MASK		(3u << 19)
#define DMA_P_XRGB8888		(0x0408213fu)
#define DMA_P_SIZE		MDP3(0x90004)
#define DMA_P_IBUF_ADDR		MDP3(0x90008)
#define DMA_P_IBUF_Y_STRIDE	MDP3(0x9000c)
#define DMA_P_OUT_XY		MDP3(0x90010)
#define DMA_P_FETCH_CFG		MDP3(0x90074)
#define MDSS_GDSCR		GCC(0x4d078)
#define DSI(off)		(chip->dsi + (off))
#define DSI_CTRL		DSI(0x004)
#define DSI_INT_CTRL8		DSI(0x110)
#define DSI_INT_MASKS		0xaaaaaaaau
#define DSI_MDP_DONE		BIT(8)
#define PLL(off)		DSI(0x300 + (off))
#define PLL_GLB_CFG		PLL(0x20)
#define PLL_LKDET_CFG2		PLL(0x64)
#define PLL_TEST_CFG		PLL(0x68)
#define PLL_CAL_CFG1		PLL(0x70)
#define PLL_STATUS		PLL(0xc0)
#define PHY_CTRL_0		0x670
#define PHY_LANES_ON		BIT(2)
#define DSI_CLAMP		(chip->io_offset + 0x0193e020)
#define DSI_PHY_RESET		(chip->io_offset + 0x0193e024)
#define DSI_CLAMPS		(BIT(9) | BIT(7))
#define DSI_CLAMP_EN		BIT(15)

static bool kicked, dsi_pending, resting;
static uint32_t intr_enable;

static bool on(void)
{
	return (readl(MDSS_GDSCR) & BIT(31)) && (readl(DMA_P_CONFIG) & DMA_P_OUT_MASK) == DMA_P_OUT_DSI_CMD &&
	       readl(DMA_P_IBUF_ADDR);
}

void mdp3_splash_size(uint32_t *w, uint32_t *h)
{
	*w = *h = 0;
	if (!on())
		return;
	uint32_t size = readl(DMA_P_SIZE);
	*w = size & 0xffff;
	*h = size >> 16;
}

bool mdp3_init(void)
{
	if (!(readl(MDSS_GDSCR) & BIT(31))) {
		log_printf("display: MDSS power domain is off\n");
		return false;
	}
	uint32_t cfg = readl(DMA_P_CONFIG), base = readl(DMA_P_IBUF_ADDR), size = readl(DMA_P_SIZE);
	log_printf("display: mdp3 rev %08lx dma_p cfg %08lx fb %08lx %lux%lu stride %lu sync %08lx sel %08lx autorefresh %08lx\n",
		   readl(HW_VERSION), cfg, base, size & 0xffff, size >> 16, readl(DMA_P_IBUF_Y_STRIDE), readl(SYNC_CONFIG_0),
		   readl(VSYNC_SEL), readl(AUTOREFRESH_CONFIG_P));
	if ((cfg & DMA_P_OUT_MASK) != DMA_P_OUT_DSI_CMD || !base) {
		log_printf("display: DMA_P is not sending to a command-mode panel\n");
		return false;
	}
	display_fb_setup(size & 0xffff, size >> 16);
	display_fb_clear();
	/* Tear check from the TE gpio (function 1, input, pull-down). With
	 * the board's line count the vsync counter is set as on the MDP5;
	 * without one the bootloader's clocks per line stay, with the full
	 * counter height. */
	writel(1 | 1 << 2, TLMM_GPIO_CFG(board->panel.te_gpio));
	uint32_t sync = 0x7ffu << 21 | (readl(SYNC_CONFIG_0) & 0x7ffff);
	if (board->panel.lines)
		sync = 0x7ffu << 21 | 19200000 / (board->panel.lines * board->panel.hz);
	writel(sync | SYNC_VSYNC_COUNTER_EN | SYNC_VSYNC_IN_EN, SYNC_CONFIG_0);
	writel(0x24, VSYNC_SEL);
	writel(fb.panel_height, PRIMARY_VSYNC_INIT_VAL);
	writel(fb.panel_height + 1, PRIMARY_RD_PTR_IRQ);
	writel(fb.panel_height, PRIMARY_START_POS);
	writel(4 | 4 << 16, SYNC_THRESH_0);
	writel(0, AUTOREFRESH_CONFIG_P);
	writel(1, TEAR_CHECK_EN);
	writel(DMA_P_XRGB8888 | DMA_P_OUT_DSI_CMD, DMA_P_CONFIG);
	writel(fb.stride * 4, DMA_P_IBUF_Y_STRIDE);
	writel(0, DMA_P_OUT_XY);
	writel(0x40, DMA_P_FETCH_CFG);
	writel(dma_addr(fb.buf[1]), DMA_P_IBUF_ADDR);
	writel(INTR_DMA_P_DONE, INTR_CLEAR);
	writel(readl(INTR_ENABLE) | INTR_DMA_P_DONE, INTR_ENABLE);
	return true;
}

bool mdp3_wait(void (*idle)(void), uint32_t *waited_us)
{
	if (!kicked)
		return true;
	uint32_t t0 = time_us();
	bool done = true;
	while (!(readl(INTR_STATUS) & INTR_DMA_P_DONE)) {
		if (idle)
			idle();
		if (time_us() - t0 > 100000) {
			done = false;
			break;
		}
	}
	*waited_us += time_us() - t0;
	writel(INTR_DMA_P_DONE, INTR_CLEAR);
	kicked = false;
	return done;
}

void mdp3_kick(const uint32_t *buf)
{
	dsb();
	writel((readl(DSI_INT_CTRL8) & DSI_INT_MASKS) | DSI_MDP_DONE, DSI_INT_CTRL8);
	writel(dma_addr(buf), DMA_P_IBUF_ADDR);
	writel(1, DMA_P_START);
	kicked = dsi_pending = true;
}

/* The vendor's idle clock-off, as its kernel does it after a few vsyncs
 * without a frame: the MDSS power domain stays on, so MDP3 keeps its
 * registers and the secure world has nothing to restore. The DSI link
 * clocks and PLL go off and the PHY is clamped, without ULPS,
 * which catfish's panel does not use; then the core clocks go. Waking,
 * the controller, PHY and PLL are written again in the vendor's order,
 * from what the bootloader set, and the PLL goes through the 8909's
 * power-up sequences until it locks. */
static uint32_t kept[128], phy_ctrl0;
static unsigned kept_n;
static bool replaying;

static void keep(uint32_t off)
{
	if (kept_n < ARRAY_SIZE(kept)) {
		if (replaying)
			writel(kept[kept_n], DSI(off));
		else
			kept[kept_n] = readl(DSI(off));
	}
	kept_n++;
}

static void ctrl_regs(void)
{
	static const uint16_t regs[] = { 0x1b8, 0x060, 0x058, 0x064, 0x05c, 0x040, 0x044, 0x084, 0x0b0,
					 0x0c4, 0x0cc, 0x0bc, 0x10c, 0x11c, 0x688, 0x050, 0x03c };
	for (unsigned i = 0; i < ARRAY_SIZE(regs); i++)
		keep(regs[i]);
}

/* The regulator reads back as zeros, so it gets the vendor's LDO settings
 * rather than what was kept. */
static void phy_regulator(void)
{
	static const struct { uint16_t off; uint8_t val; } ldo[] = {
		{ 0x794, 0x07 }, { 0x78c, 0x00 }, { 0x788, 0x01 }, { 0x784, 0x01 }, { 0x790, 0x20 },
	};
	writel(0, DSI(0x780));
	writel(0, DSI(0x798));
	delay_us(1000);
	for (unsigned i = 0; i < ARRAY_SIZE(ldo); i++)
		writel(ldo[i].val, DSI(ldo[i].off));
}

/* The regulator, then the PHY with its lanes enabled last. */
static void phy_regs(void)
{
	if (replaying)
		phy_regulator();
	keep(0x6dc);
	keep(0x684);
	for (uint32_t off = 0x640; off <= 0x66c; off += 4)
		keep(off);
	for (uint32_t lane = 0; lane < 5; lane++)
		for (uint32_t w = 0; w < 9; w++)
			keep(0x500 + lane * 0x40 + w * 4);
	keep(0x680);
	keep(0x6d4);
	keep(PHY_CTRL_0);
	for (uint32_t off = 0x6b4; off <= 0x6c8; off += 4)
		keep(off);
}

/* The VCO's configuration in the vendor's order. Lock detect is set to
 * its starting value rather than the one the power-up left. */
static void pll_regs(void)
{
	static const uint8_t first[] = { 0x2c, 0x30, 0x34, 0x08, 0x78, 0x7c };
	static const uint8_t sdm[] = { 0x3c, 0x40, 0x44, 0x48 };
	static const uint8_t then[] = { 0x00, 0x14, 0x0c, 0x38, 0x6c, 0x84, 0x88, 0x8c, 0x90, 0x94, 0x98, 0x9c };
	for (unsigned i = 0; i < ARRAY_SIZE(first); i++)
		keep(0x300 + first[i]);
	if (replaying)
		writel(0x0d, PLL_LKDET_CFG2);
	for (unsigned i = 0; i < ARRAY_SIZE(sdm); i++)
		keep(0x300 + sdm[i]);
	if (replaying)
		delay_us(1000);
	for (unsigned i = 0; i < ARRAY_SIZE(then); i++)
		keep(0x300 + then[i]);
}

static void pll_dividers(void)
{
	static const uint8_t regs[] = { 0x04, 0x10, 0x24, 0x28 };
	for (unsigned i = 0; i < ARRAY_SIZE(regs); i++)
		keep(0x300 + regs[i]);
}

static bool save_regs(void)
{
	replaying = false;
	kept_n = 0;
	ctrl_regs();
	keep(0x110);
	keep(0x004);
	phy_regs();
	pll_regs();
	pll_dividers();
	if (kept_n > ARRAY_SIZE(kept)) {
		log_printf("display: %u registers to keep, room for %u\n", kept_n, (unsigned)ARRAY_SIZE(kept));
		return false;
	}
	return true;
}

/* TSMC, GF part 1 and GF part 2, three tries each. */
static bool pll_power_up(void)
{
	static const uint8_t cal1[] = { 0x34, 0x14, 0x04 };
	for (unsigned s = 0; s < 9; s++) {
		writel(1, PLL_TEST_CFG);
		delay_us(1);
		writel(0, PLL_TEST_CFG);
		writel(cal1[s / 3], PLL_CAL_CFG1);
		writel(0x01, PLL_GLB_CFG);
		writel(0x05, PLL_GLB_CFG);
		if (s >= 3)
			delay_us(3);
		writel(0x0f, PLL_GLB_CFG);
		delay_us(500);
		writel(0x04, PLL_LKDET_CFG2);
		delay_us(1);
		writel(0x05, PLL_LKDET_CFG2);
		delay_us(512);
		for (int i = 0; i <= 10; i++) {
			if (readl(PLL_STATUS) & 1) {
				if (s)
					log_printf("display: DSI PLL locked with sequence %u\n", s);
				return true;
			}
			delay_us(50);
		}
	}
	return false;
}

static void link_clocks(bool on)
{
	static const uint32_t off_order[] = { 0x4d098, 0x4d084, 0x4d094 };
	for (unsigned i = 0; i < ARRAY_SIZE(off_order); i++)
		display_clock_set(GCC(off_order[on ? ARRAY_SIZE(off_order) - 1 - i : i]), on);
}

static void core_clocks(bool on)
{
	static const uint32_t off_order[] = { 0x4d080, 0x4d07c, 0x4d088, 0x4d090 };
	for (unsigned i = 0; i < ARRAY_SIZE(off_order); i++)
		display_clock_set(GCC(off_order[on ? ARRAY_SIZE(off_order) - 1 - i : i]), on);
}

bool mdp3_rest(void)
{
	if (resting)
		return true;
	if (kicked)
		return false;
	uint32_t t0 = time_us();
	while (dsi_pending && !(readl(DSI_INT_CTRL8) & DSI_MDP_DONE)) {
		if (time_us() - t0 > 20000) {
			log_printf("display: frame not done on DSI, not resting\n");
			return false;
		}
	}
	dsi_pending = false;
	if (!save_regs())
		return false;
	phy_ctrl0 = readl(DSI(PHY_CTRL_0));
	link_clocks(false);
	writel(0, PLL_GLB_CFG);
	uint32_t clamp = readl(DSI_CLAMP);
	writel(clamp | DSI_CLAMPS, DSI_CLAMP);
	writel(clamp | DSI_CLAMPS | DSI_CLAMP_EN, DSI_CLAMP);
	writel(1, DSI_PHY_RESET);
	intr_enable = readl(INTR_ENABLE);
	writel(0, INTR_ENABLE);
	dsb();
	core_clocks(false);
	resting = true;
	return true;
}

void mdp3_active(void)
{
	if (!resting)
		return;
	core_clocks(true);
	writel(intr_enable, INTR_ENABLE);
	replaying = true;
	kept_n = 0;
	ctrl_regs();
	writel(kept[kept_n++] & DSI_INT_MASKS, DSI_INT_CTRL8);
	writel(kept[kept_n++], DSI_CTRL);
	writel(0, DSI_PHY_RESET);
	writel(readl(DSI_CLAMP) & ~(DSI_CLAMPS | DSI_CLAMP_EN), DSI_CLAMP);
	writel(phy_ctrl0 & ~PHY_LANES_ON, DSI(PHY_CTRL_0));
	phy_regs();
	/* A PLL that did not lock gets its configuration once more. */
	unsigned pll_at = kept_n;
	bool locked = false;
	for (int tries = 0; tries < 2 && !locked; tries++) {
		kept_n = pll_at;
		writel(1, PLL_TEST_CFG);
		delay_us(1000);
		writel(0, PLL_TEST_CFG);
		delay_us(1000);
		pll_regs();
		locked = pll_power_up();
	}
	if (!locked)
		log_printf("display: DSI PLL not locked, status %08lx\n", readl(PLL_STATUS));
	pll_dividers();
	link_clocks(true);
	resting = false;
}
