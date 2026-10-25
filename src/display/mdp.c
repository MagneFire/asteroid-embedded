/* The bootloader leaves the panel on with the MDP5 pipe scanning a
 * framebuffer it allocated. Read the pipe configuration back instead of
 * programming the display from scratch: base, stride, size and format
 * come from the source pipe registers. The panel runs in DSI command mode,
 * so each new frame is pushed by a CTL_START kick unless the ping-pong
 * block's autorefresh is on. */
#include <string.h>
#include "asteroid.h"
#include "display.h"

struct fb fb;
static bool kick_needed;
static uint32_t last_kick;

static const uint32_t pipes[] = { MDP_PIPE_VIG0, MDP_PIPE_RGB0, MDP_PIPE_DMA0 };

bool display_init(void)
{
	if (!(readl(MDP_GDSCR) & BIT(31))) {
		log_printf("display: MDP power domain is off\n");
		return false;
	}
	uint32_t pipe = 0, base = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(pipes); i++) {
		base = readl(pipes[i] + PIPE_SRC0_ADDR);
		if (base) {
			pipe = pipes[i];
			break;
		}
	}
	if (!pipe) {
		log_printf("display: no pipe has a source buffer\n");
		return false;
	}
	uint32_t img = readl(pipe + PIPE_SRC_IMG_SIZE);
	uint32_t stride = readl(pipe + PIPE_SRC_YSTRIDE);
	uint32_t format = readl(pipe + PIPE_SRC_FORMAT);
	uint32_t bpp = ((format >> 9) & 3) + 1;
	uint32_t top = readl(MDP_CTL0 + CTL_TOP);
	uint32_t ar = readl(MDP_PP0 + PP_AUTOREFRESH_CONFIG);
	fb.base = (void *)(uintptr_t)base;
	fb.width = img & 0xffff;
	fb.height = img >> 16;
	fb.stride = stride / bpp;
	fb.bpp = bpp;
	bool cmd_mode = top & BIT(17);
	kick_needed = cmd_mode && !(ar & BIT(31));
	log_printf("display: mdp rev %08lx pipe %08lx fb %08lx %lux%lu stride %lu bpp %lu format %08lx %s autorefresh %lu\n",
		   readl(MDP_HW_REV), pipe, base, fb.width, fb.height, fb.stride, bpp, format,
		   cmd_mode ? "cmd" : "video", (ar >> 31) & 1);
	return true;
}

void display_update(void)
{
	if (!kick_needed || !fb.base)
		return;
	uint32_t now = time_ms();
	if (now - last_kick < 20)
		delay_ms(20 - (now - last_kick));
	dsb();
	writel(1, MDP_CTL0 + CTL_START);
	last_kick = time_ms();
}
