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

static bool kicked;

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
	writel(dma_addr(buf), DMA_P_IBUF_ADDR);
	writel(1, DMA_P_START);
	kicked = true;
}
