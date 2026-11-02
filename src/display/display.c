/* The bootloader leaves the DSI command-mode panel on with MDP5 pipe RGB_0
 * scanning a framebuffer it allocated. The pipe is re-pointed at two
 * buffers of the firmware's own, in XRGB8888, flipped vertically to match
 * the panel's orientation, with rows padded to a multiple of 32 pixels as
 * the GPU's render target pitch requires. A frame is pushed by flushing the pipe and
 * kicking CTL_START; the ping-pong block waits for the panel's tearing
 * effect line, so kicks are paced by waiting for the previous frame's
 * PP_DONE before the next one. Nothing is kicked until the first frame, so
 * the panel keeps showing the bootloader's splash until then. */
#include <string.h>
#include "asteroid.h"
#include "display.h"

#define DSI_MDP_DONE		BIT(8)

struct fb fb;
static bool cmd_mode, kicked, dsi_pending;
static uint32_t frames, waits, timeouts;
static void (*idle_hook)(void);
void display_set_idle_hook(void (*fn)(void)) { idle_hook = fn; }
uint32_t display_timeouts(void) { return timeouts; }

#define SRC_FORMAT_XRGB8888	(0x3 << 9 | 0x3 << 6 | 0x3 << 4 | 0x3 << 2 | 0x3 << 0 | 0x1 << 17 | 0x3 << 12)
#define UNPACK_XRGB		(0x3 << 24 | 0x2 << 16 | 0x0 << 8 | 0x1 << 0)
#define PIPE			MDP_PIPE_RGB0

static void pp_wait(void)
{
	if (!kicked)
		return;
	uint32_t t0 = time_us();
	while (!(readl(MDP_INTR_STATUS) & MDP_INTR_PP0_DONE)) {
		if (idle_hook)
			idle_hook();
		if (time_us() - t0 > 100000) {
			timeouts++;
			break;
		}
	}
	waits += time_us() - t0;
	writel(MDP_INTR_PP0_DONE, MDP_INTR_CLEAR);
	kicked = false;
}

static void kick(void)
{
	dsb();
	writel((readl(DSI_INT_CTRL) & 0x02020202) | DSI_MDP_DONE, DSI_INT_CTRL);
	dsi_pending = true;
	writel(CTL_FLUSH_RGB0, MDP_CTL0 + CTL_FLUSH);
	writel(1, MDP_CTL0 + CTL_START);
	kicked = true;
	frames++;
}

/* The size of the picture the bootloader left on the panel, 0 x 0 if
 * the display is off; it is the panel's own size. */
void display_splash_size(uint32_t *w, uint32_t *h)
{
	*w = *h = 0;
	if (!(readl(MDP_GDSCR) & BIT(31)) || !readl(PIPE + PIPE_SRC0_ADDR))
		return;
	uint32_t img = readl(PIPE + PIPE_SRC_IMG_SIZE);
	*w = img & 0xffff;
	*h = img >> 16;
}

bool display_init(void)
{
	if (!(readl(MDP_GDSCR) & BIT(31))) {
		log_printf("display: MDP power domain is off\n");
		return false;
	}
	uint32_t base = readl(PIPE + PIPE_SRC0_ADDR);
	if (!base) {
		log_printf("display: pipe RGB_0 has no source buffer\n");
		return false;
	}
	uint32_t img = readl(PIPE + PIPE_SRC_IMG_SIZE);
	uint32_t format = readl(PIPE + PIPE_SRC_FORMAT);
	uint32_t top = readl(MDP_CTL0 + CTL_TOP);
	uint32_t ar = readl(MDP_PP0 + PP_AUTOREFRESH_CONFIG);
	fb.width = img & 0xffff;
	fb.height = img >> 16;
	fb.stride = (fb.width + 31) & ~31u;
	fb.buf[0] = (uint32_t *)(uintptr_t)FB_BASE;
	fb.buf[1] = (uint32_t *)(uintptr_t)(FB_BASE + fb.stride * fb.height * 4);
	fb.back = 0;
	cmd_mode = top & BIT(17);
	log_printf("display: mdp rev %08lx fb %08lx %lux%lu format %08lx %s autorefresh %lu op_mode %08lx\n",
		   readl(MDP_HW_REV), base, fb.width, fb.height, format,
		   cmd_mode ? "cmd" : "video", (ar >> 31) & 1, readl(PIPE + PIPE_SRC_OP_MODE));
	log_printf("display: pp tear %lu sync %08lx height %lu init %lu thresh %08lx start %lu rdptr %lu, te gpio cfg %08lx\n",
		   readl(MDP_PP0 + PP_TEAR_CHECK_EN), readl(MDP_PP0 + PP_SYNC_CONFIG_VSYNC),
		   readl(MDP_PP0 + PP_SYNC_CONFIG_HEIGHT), readl(MDP_PP0 + PP_VSYNC_INIT_VAL),
		   readl(MDP_PP0 + PP_SYNC_THRESH), readl(MDP_PP0 + PP_START_POS),
		   readl(MDP_PP0 + PP_RD_PTR_IRQ), readl(TLMM_GPIO_CFG(board->panel.te_gpio)));
	/* Tear check: the TE gpio carries the panel's TE line into the MDP
	 * (function 1, input, pull-down). The vsync counter runs at 19.2 MHz
	 * over the panel's lines (height + porches) at its refresh rate and
	 * is resynchronised by TE; a kick then starts at the vertical blank. */
	writel(1 | 1 << 2, TLMM_GPIO_CFG(board->panel.te_gpio));
	uint32_t edges = 0, level = readl(TLMM_GPIO_IO(board->panel.te_gpio)) & 1, t0 = time_us();
	while (time_us() - t0 < 100000) {
		uint32_t l = readl(TLMM_GPIO_IO(board->panel.te_gpio)) & 1;
		if (l != level) {
			edges++;
			level = l;
		}
	}
	log_printf("display: te gpio %lu edges in 100 ms\n", edges);
	uint32_t vclks_line = 19200000 / (board->panel.lines * board->panel.hz);
	writel(PP_VSYNC_COUNTER_EN | PP_VSYNC_IN_EN | vclks_line, MDP_PP0 + PP_SYNC_CONFIG_VSYNC);
	writel(board->panel.lines, MDP_PP0 + PP_SYNC_CONFIG_HEIGHT);
	writel(fb.height, MDP_PP0 + PP_VSYNC_INIT_VAL);
	writel(fb.height + 1, MDP_PP0 + PP_RD_PTR_IRQ);
	writel(fb.height, MDP_PP0 + PP_START_POS);
	writel(4 | 4 << 16, MDP_PP0 + PP_SYNC_THRESH);
	writel(0, MDP_PP0 + PP_AUTOREFRESH_CONFIG);
	writel(1, MDP_PP0 + PP_TEAR_CHECK_EN);
	memset(fb.buf[0], 0, fb.stride * fb.height * 4);
	memset(fb.buf[1], 0, fb.stride * fb.height * 4);
	dcache_clean_range(fb.buf[0], fb.stride * fb.height * 8);
	writel(SRC_FORMAT_XRGB8888, PIPE + PIPE_SRC_FORMAT);
	writel(UNPACK_XRGB, PIPE + PIPE_SRC_UNPACK);
	writel(fb.stride * 4, PIPE + PIPE_SRC_YSTRIDE);
	writel((readl(PIPE + PIPE_SRC_OP_MODE) & ~(PIPE_FLIP_LR | PIPE_FLIP_UD)) | (board->panel.flip_ud ? PIPE_FLIP_UD : 0),
	       PIPE + PIPE_SRC_OP_MODE);
	writel((uint32_t)(uintptr_t)fb.buf[1], PIPE + PIPE_SRC0_ADDR);
	writel(MDP_INTR_PP0_DONE, MDP_INTR_CLEAR);
	return true;
}

uint32_t *display_backbuffer(void) { return fb.buf[fb.back]; }

/* While the screen sleeps, between frames, the display rests as the
 * vendor's idle power collapse leaves it: the last frame done on the
 * DSI side too, the clock lane and data lane 0 in ultra-low-power state,
 * the registers the power domain loses saved, the DSI PHY clamped so the
 * lanes hold their state (not with the panel asleep), the DSI and MDP
 * clocks gated, the DSI PLL and the register bus clock off and the MDSS
 * power domain collapsed, so nothing in MDSS reads memory when the
 * cluster powers down and XO stops.
 * Waking, the power domain comes back with its clocks, then the secure
 * world restores its SMMU configuration, without which no MDSS register
 * answers, and the saved registers are written back. The controller is
 * put back into ULPS to match the PHY before the clamps are released,
 * the PLL goes through its power-up sequence until it reports lock, then
 * escape, byte and pixel follow; leaving ULPS takes 1 ms after clearing
 * lane 0's PHY errors, then the lanes are forced to the stop state.
 * display_rest says whether the cluster may power down: not while a
 * frame is still on its way to the panel; if only the lanes refuse
 * ULPS, the display stays clocked and the cluster may still go. */
#define ULPS_LANES		(BIT(4) | BIT(0))
#define CBCR_HW_CTL		BIT(1)
#define CBCR_OFF		BIT(31)
#define GDSC_SW_COLLAPSE	BIT(0)
#define GDSC_PWR_ON		BIT(31)
/* Clamps for the clock lane and data lane 0 (lane map 3012) with their
 * ULPS bits, and the bit that applies them. */
#define DSI_CLAMPS		(BIT(9) | BIT(8) | BIT(5) | BIT(4))
#define DSI_CLAMP_EN		BIT(15)

static const uint32_t mdp_clocks[] = { MDSS_AXI_CBCR, MDSS_MDP_CBCR, MDSS_MDP_LUT_CBCR, MDSS_VSYNC_CBCR };
static const uint32_t dsi_clocks[] = { MDSS_ESC0_CBCR, MDSS_BYTE0_CBCR, MDSS_PCLK0_CBCR };
static bool resting, panel_asleep;

/* The registers the MDSS power domain loses when it collapses, in the
 * order they are written back: VBIF, MDP top (interface select, the
 * shared memory pool, QoS), CTL0, pipe RGB0, mixer 0, INTF1, ping-pong 0
 * with its tear check enabled last, the DSI PLL's configuration, the DSI
 * PHY with its regulator first and its lanes enabled last, and the DSI
 * controller, enabled last. Status, interrupt and trigger registers are
 * left out. */
static const uint32_t kept_regs[] = {
	0xfd924004, 0xfd9240d8,
	0xfd900104, 0xfd900180, 0xfd900230, 0xfd9002e0, 0xfd9003ac, 0xfd9003b4, 0xfd9004a8, 0xfd9004b0,
	0xfd900600, 0xfd900614,
	0xfd901e00, 0xfd901e04, 0xfd901e08, 0xfd901e0c, 0xfd901e10, 0xfd901e14, 0xfd901e24, 0xfd901e30,
	0xfd901e34, 0xfd901e38, 0xfd901e50, 0xfd901e54, 0xfd901e58, 0xfd901ed0, 0xfd901ee0,
	0xfd903204, 0xfd903220, 0xfd903224, 0xfd903250, 0xfd903254, 0xfd903280, 0xfd903284, 0xfd9032b0, 0xfd9032b4,
	0xfd921390,
	0xfd921b04, 0xfd921b08, 0xfd921b0c, 0xfd921b10, 0xfd921b18, 0xfd921b1c, 0xfd921b20, 0xfd921b30, 0xfd921b00,
	0xfd922a00, 0xfd922a04, 0xfd922a08, 0xfd922a0c, 0xfd922a10, 0xfd922a14, 0xfd922a18, 0xfd922a1c,
	0xfd922a24, 0xfd922a28, 0xfd922a2c, 0xfd922a30, 0xfd922a34, 0xfd922a38, 0xfd922a3c, 0xfd922a40,
	0xfd922a44, 0xfd922a48, 0xfd922a4c, 0xfd922a50, 0xfd922a54, 0xfd922a58, 0xfd922a5c, 0xfd922a60,
	0xfd922a6c, 0xfd922a70, 0xfd922a74, 0xfd922a78, 0xfd922a7c, 0xfd922a80, 0xfd922a84, 0xfd922a88,
	0xfd922a8c, 0xfd922a90, 0xfd922a94, 0xfd922a98, 0xfd922a9c,
	0xfd922d80, 0xfd922d84, 0xfd922d88, 0xfd922d8c, 0xfd922d90, 0xfd922d94, 0xfd922d98,
	0xfd922b04, 0xfd922b1c, 0xfd922b20, 0xfd922b30, 0xfd922b44, 0xfd922b50, 0xfd922b5c, 0xfd922b60,
	0xfd922b70, 0xfd922b84, 0xfd922b90, 0xfd922b9c, 0xfd922ba0, 0xfd922bb0, 0xfd922bc4, 0xfd922bd0,
	0xfd922bdc, 0xfd922be0, 0xfd922bf0, 0xfd922c04, 0xfd922c1c, 0xfd922c20, 0xfd922c30,
	0xfd922c40, 0xfd922c44, 0xfd922c48, 0xfd922c50, 0xfd922c54, 0xfd922c58, 0xfd922c5c, 0xfd922c60,
	0xfd922c64, 0xfd922c84, 0xfd922c88, 0xfd922ce0,
	0xfd922db0, 0xfd922db4, 0xfd922db8, 0xfd922dc0, 0xfd922dc4, 0xfd922dc8, 0xfd922dd0, 0xfd922dd4,
	0xfd922dd8, 0xfd922df0, 0xfd922df4, 0xfd922df8, 0xfd922d20,
	0xfd922c70,
	0xfd92283c, 0xfd922840, 0xfd922844, 0xfd922848, 0xfd92284c, 0xfd922858, 0xfd92285c, 0xfd922860,
	0xfd922864, 0xfd92286c, 0xfd922884, 0xfd922890, 0xfd922894, 0xfd9228a0, 0xfd9228b0, 0xfd9228c4,
	0xfd9228cc, 0xfd9228f0, 0xfd92290c, 0xfd92291c,
	0xfd922804,
};
static uint32_t kept[ARRAY_SIZE(kept_regs)];

static void clock_set(uint32_t cbcr, bool on)
{
	uint32_t v = readl(cbcr);
	if (v & CBCR_HW_CTL)
		log_printf("display: clock %08lx under hardware control\n", cbcr);
	writel(on ? v | 1 : v & ~1u, cbcr);
	for (int i = 0; !(readl(cbcr) & CBCR_OFF) != on; i++) {
		if (i == 1000) {
			log_printf("display: clock %08lx stuck %s, %08lx\n", cbcr, on ? "off" : "on", readl(cbcr));
			return;
		}
		delay_us(1);
	}
}

static bool pll_locked(void)
{
	writel(0x0d, DSI_PLL_LKDET_CFG2);
	writel(0x0c, DSI_PLL_LKDET_CFG2);
	delay_us(1);
	writel(0x0d, DSI_PLL_LKDET_CFG2);
	for (int i = 0; i < 10; i++) {
		if (readl(DSI_PLL_STATUS) & 1)
			return true;
		delay_us(50);
	}
	return false;
}

/* The PLL's power-up sequences are the vendor's for msm8226, tried in
 * its order until one locks; each starts with a software reset, which
 * keeps the divider settings. The first one retries the last steps with
 * the power generator reset; the others reset it first. */
struct pll_step { uint8_t reg, val; uint16_t us; };
#define GLB	0x20
#define PWRGEN	0x14
#define CAL1	0x70
static const struct pll_step seq_m[] = { { CAL1, 0x34, 0 }, { GLB, 0x01, 200 }, { GLB, 0x05, 200 }, { GLB, 0x0f, 600 } };
static const struct pll_step seq_m_retry[] = { { PWRGEN, 0x00, 50 }, { GLB, 0x05, 100 }, { GLB, 0x0f, 600 } };
static const struct pll_step seq_d[] = { { PWRGEN, 0x00, 50 }, { GLB, 0x01, 200 }, { GLB, 0x05, 200 }, { GLB, 0x07, 200 },
					 { GLB, 0x05, 200 }, { GLB, 0x07, 200 }, { GLB, 0x0f, 600 } };
static const struct pll_step seq_f1[] = { { PWRGEN, 0x00, 50 }, { GLB, 0x01, 200 }, { GLB, 0x05, 200 }, { GLB, 0x0f, 200 },
					  { GLB, 0x0d, 200 }, { GLB, 0x0f, 600 } };
static const struct pll_step seq_c[] = { { PWRGEN, 0x00, 50 }, { GLB, 0x01, 200 }, { GLB, 0x05, 200 }, { GLB, 0x0f, 600 } };
static const struct pll_step seq_e[] = { { PWRGEN, 0x00, 50 }, { GLB, 0x01, 200 }, { GLB, 0x05, 200 }, { GLB, 0x0d, 1 },
					 { GLB, 0x0f, 600 } };
static const struct { const struct pll_step *steps; uint8_t n, retries; } pll_seqs[] = {
	{ seq_m, ARRAY_SIZE(seq_m), 7 }, { seq_m, ARRAY_SIZE(seq_m), 7 },
	{ seq_d, ARRAY_SIZE(seq_d), 0 }, { seq_d, ARRAY_SIZE(seq_d), 0 },
	{ seq_f1, ARRAY_SIZE(seq_f1), 0 }, { seq_c, ARRAY_SIZE(seq_c), 0 }, { seq_e, ARRAY_SIZE(seq_e), 0 },
};

static void pll_steps(const struct pll_step *steps, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		writel(steps[i].val, DSI_PLL_GLB_CFG - GLB + steps[i].reg);
		if (steps[i].us)
			delay_us(steps[i].us);
	}
}

static void pll_power_up(void)
{
	for (unsigned s = 0; s < ARRAY_SIZE(pll_seqs); s++) {
		writel(1, DSI_PLL_TEST_CFG);
		delay_us(1);
		writel(0, DSI_PLL_TEST_CFG);
		delay_us(1);
		pll_steps(pll_seqs[s].steps, pll_seqs[s].n);
		bool locked = pll_locked();
		unsigned r = 0;
		for (; !locked && r < pll_seqs[s].retries; r++) {
			pll_steps(seq_m_retry, ARRAY_SIZE(seq_m_retry));
			locked = pll_locked();
		}
		if (locked) {
			if (s || r)
				log_printf("display: DSI PLL locked with sequence %u after %u retries\n", s, r);
			return;
		}
	}
	log_printf("display: DSI PLL not locked, status %08lx\n", readl(DSI_PLL_STATUS));
}

bool display_rest(void)
{
	if (resting)
		return true;
	pp_wait();
	uint32_t t0 = time_us();
	while (dsi_pending && !(readl(DSI_INT_CTRL) & DSI_MDP_DONE)) {
		if (time_us() - t0 > 100000) {
			log_printf("display: frame not done on DSI, not resting\n");
			return false;
		}
	}
	dsi_pending = false;
	writel(ULPS_LANES, DSI_LANE_CTRL);
	delay_us(100);
	if (readl(DSI_LANE_STATUS) & ULPS_LANES << 8) {
		log_printf("display: lanes not in ULPS, status %08lx, not resting\n", readl(DSI_LANE_STATUS));
		writel(0, DSI_LANE_CTRL);
		return true;
	}
	for (unsigned i = 0; i < ARRAY_SIZE(kept_regs); i++)
		kept[i] = readl(kept_regs[i]);
	if (!panel_asleep) {
		clock_set(MMSS_MISC_AHB_CBCR, true);
		writel(readl(MMSS_DSI_CLAMP) | DSI_CLAMPS, MMSS_DSI_CLAMP);
		writel(readl(MMSS_DSI_CLAMP) | DSI_CLAMPS | DSI_CLAMP_EN, MMSS_DSI_CLAMP);
		writel(1, MMSS_DSI_PHY_RESET);
		clock_set(MMSS_MISC_AHB_CBCR, false);
	}
	for (unsigned i = 0; i < ARRAY_SIZE(dsi_clocks); i++)
		clock_set(dsi_clocks[i], false);
	for (unsigned i = 0; i < ARRAY_SIZE(mdp_clocks); i++)
		clock_set(mdp_clocks[i], false);
	writel(0, DSI_PLL_GLB_CFG);
	dsb();
	clock_set(MDSS_AHB_CBCR, false);
	writel(readl(MDP_GDSCR) | GDSC_SW_COLLAPSE, MDP_GDSCR);
	for (int i = 0; i < 1000 && (readl(MDP_GDSCR) & GDSC_PWR_ON); i++)
		delay_us(1);
	resting = true;
	return true;
}

void display_active(void)
{
	if (!resting)
		return;
	writel(readl(MDP_GDSCR) & ~GDSC_SW_COLLAPSE, MDP_GDSCR);
	for (int i = 0; !(readl(MDP_GDSCR) & GDSC_PWR_ON); i++) {
		if (i == 1000) {
			log_printf("display: power domain stays off, %08lx\n", readl(MDP_GDSCR));
			break;
		}
		delay_us(1);
	}
	delay_us(1);
	clock_set(MDSS_AHB_CBCR, true);
	for (unsigned i = 0; i < ARRAY_SIZE(mdp_clocks); i++)
		clock_set(mdp_clocks[i], true);
	int r = scm_restore_sec_cfg(1);
	if (r)
		log_printf("display: secure config not restored (%d)\n", r);
	for (unsigned i = 0; i < ARRAY_SIZE(kept_regs); i++)
		writel(kept[i], kept_regs[i]);
	if (!panel_asleep) {
		clock_set(MDSS_ESC0_CBCR, true);
		writel(ULPS_LANES, DSI_LANE_CTRL);
		delay_us(100);
		clock_set(MDSS_ESC0_CBCR, false);
		clock_set(MMSS_MISC_AHB_CBCR, true);
		writel(0, MMSS_DSI_PHY_RESET);
		writel(readl(MMSS_DSI_CLAMP) & ~(DSI_CLAMPS | DSI_CLAMP_EN), MMSS_DSI_CLAMP);
		clock_set(MMSS_MISC_AHB_CBCR, false);
	}
	pll_power_up();
	for (unsigned i = 0; i < ARRAY_SIZE(dsi_clocks); i++)
		clock_set(dsi_clocks[i], true);
	uint32_t err = readl(DSI_DLN0_PHY_ERR);
	if (err & 0x011111)
		writel(err, DSI_DLN0_PHY_ERR);
	writel(ULPS_LANES << 8, DSI_LANE_CTRL);
	delay_us(1000);
	writel(ULPS_LANES << 16, DSI_LANE_CTRL);
	writel(0, DSI_LANE_CTRL);
	delay_us(100);
	resting = false;
}

void display_flip(void)
{
	display_active();
	uint32_t *buf = fb.buf[fb.back];
	dcache_clean_range(buf, fb.stride * fb.height * 4);
	pp_wait();
	writel((uint32_t)(uintptr_t)buf, PIPE + PIPE_SRC0_ADDR);
	kick();
	fb.back ^= 1;
}

void display_wait_idle(void) { pp_wait(); }
uint32_t display_frames(void) { return frames; }
uint32_t display_waits(void) { return waits; }

void display_set_brightness(uint8_t level)
{
	display_active();
	pp_wait();
	dsi_dcs_write(0x51, &level, 1);
}

void display_sleep(void)
{
	display_active();
	pp_wait();
	dsi_dcs_write(0x28, NULL, 0);
	dsi_dcs_write(0x10, NULL, 0);
	panel_asleep = true;
}

void display_wake(void)
{
	display_active();
	panel_asleep = false;
	dsi_dcs_write(0x11, NULL, 0);
	delay_ms(120);
	dsi_dcs_write(0x29, NULL, 0);
}

/* Idle mode keeps the panel showing its memory with fewer colours and
 * less power, for the always-on face. The command sequences are the
 * panel's, from the board. */
static void dcs_sequence(const struct dcs_step *steps, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		dsi_dcs_write(steps[i].cmd, steps[i].data, steps[i].n);
		if (steps[i].delay_ms)
			delay_ms(steps[i].delay_ms);
	}
}

void display_idle(bool on)
{
	display_active();
	pp_wait();
	if (on)
		dcs_sequence(board->panel.idle_on, board->panel.idle_on_n);
	else
		dcs_sequence(board->panel.idle_off, board->panel.idle_off_n);
}
