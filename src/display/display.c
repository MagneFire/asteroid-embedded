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
		   readl(MDP_PP0 + PP_RD_PTR_IRQ), readl(TLMM_GPIO_CFG(24)));
	/* Tear check: gpio 24 carries the panel's TE line into the MDP
	 * (function 1, input, pull-down). The vsync counter runs at 19.2 MHz
	 * over the panel's 434 lines (400 + porches) at 60 Hz and is
	 * resynchronised by TE; a kick then starts at the vertical blank. */
	writel(1 | 1 << 2, TLMM_GPIO_CFG(24));
	uint32_t edges = 0, level = readl(TLMM_GPIO_IO(24)) & 1, t0 = time_us();
	while (time_us() - t0 < 100000) {
		uint32_t l = readl(TLMM_GPIO_IO(24)) & 1;
		if (l != level) {
			edges++;
			level = l;
		}
	}
	log_printf("display: te gpio %lu edges in 100 ms\n", edges);
	uint32_t vclks_line = 19200000 / (434 * 60);
	writel(PP_VSYNC_COUNTER_EN | PP_VSYNC_IN_EN | vclks_line, MDP_PP0 + PP_SYNC_CONFIG_VSYNC);
	writel(434, MDP_PP0 + PP_SYNC_CONFIG_HEIGHT);
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
	writel((readl(PIPE + PIPE_SRC_OP_MODE) & ~(PIPE_FLIP_LR | PIPE_FLIP_UD)) | PIPE_FLIP_UD, PIPE + PIPE_SRC_OP_MODE);
	writel((uint32_t)(uintptr_t)fb.buf[1], PIPE + PIPE_SRC0_ADDR);
	writel(MDP_INTR_PP0_DONE, MDP_INTR_CLEAR);
	return true;
}

uint32_t *display_backbuffer(void) { return fb.buf[fb.back]; }

/* While the screen sleeps, between frames, the display rests as the
 * vendor's command-mode idle path leaves it: the last frame done on the
 * DSI side too, the clock lane and data lane 0 in ultra-low-power state
 * and the MDP and DSI clocks gated, so nothing in MDSS reads memory
 * when the cluster powers down and XO stops. The power domain stays on
 * and keeps the registers the bootloader set up. Waking, the bus and
 * MDP clocks come back first, then escape, byte and pixel once the DSI
 * PLL reports lock again; leaving ULPS takes 1 ms after clearing lane
 * 0's PHY errors, then the lanes are forced to the stop state.
 * display_rest says whether the cluster may power down: not while a
 * frame is still on its way to the panel; if only the lanes refuse
 * ULPS, the display stays clocked and the cluster may still go. */
#define ULPS_LANES		(BIT(4) | BIT(0))
#define CBCR_HW_CTL		BIT(1)
#define CBCR_OFF		BIT(31)

static const uint32_t mdp_clocks[] = { MDSS_AXI_CBCR, MDSS_MDP_CBCR, MDSS_MDP_LUT_CBCR, MDSS_VSYNC_CBCR };
static const uint32_t dsi_clocks[] = { MDSS_ESC0_CBCR, MDSS_BYTE0_CBCR, MDSS_PCLK0_CBCR };
static bool resting;

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
	for (unsigned i = 0; i < ARRAY_SIZE(dsi_clocks); i++)
		clock_set(dsi_clocks[i], false);
	for (unsigned i = 0; i < ARRAY_SIZE(mdp_clocks); i++)
		clock_set(mdp_clocks[i], false);
	resting = true;
	return true;
}

void display_active(void)
{
	if (!resting)
		return;
	for (unsigned i = 0; i < ARRAY_SIZE(mdp_clocks); i++)
		clock_set(mdp_clocks[i], true);
	if (!pll_locked())
		log_printf("display: DSI PLL not locked, status %08lx\n", readl(DSI_PLL_STATUS));
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
}

void display_wake(void)
{
	display_active();
	dsi_dcs_write(0x11, NULL, 0);
	delay_ms(120);
	dsi_dcs_write(0x29, NULL, 0);
}

/* Idle mode keeps the panel showing its memory with fewer colours and
 * less power, for the always-on face. Entering it, the vendor selects
 * command page 0 and sends a NOP followed by 150 ms. */
void display_idle(bool on)
{
	display_active();
	static const uint8_t zero = 0;
	pp_wait();
	dsi_dcs_write(0xfe, &zero, 1);
	if (on) {
		dsi_dcs_write(0x00, &zero, 1);
		delay_ms(150);
		dsi_dcs_write(0x39, NULL, 0);
	} else {
		dsi_dcs_write(0x38, NULL, 0);
	}
}
