/* Drawing a frame where there are no layers to compose: the screen on
 * show renders what changed in it straight to the panel (ui_draw), the
 * overlay instead while it is up. A move between screens is a switch. */
#include "asteroid.h"
#include "display/display.h"
#include "display/flatmesh_mcu.h"
#include "ui/ui.h"
#include "ui/stack.h"
#include "power.h"
#include "frame.h"
#include "sched.h"

static bool have_ui, woke;
static uint32_t hold_t, frames, draws, draw_us;

void frame_init(bool ui, bool gpu, uint32_t start)
{
	have_ui = ui;
	flatmesh_mcu_init(board->panel.width, board->panel.height);
}
void frame_busy(uint32_t now) {}
void frame_touch_dot(int x, int y) {}
void frame_app_colors(const struct ui_colors *c) {}
void frame_sleep(void) {}
void frame_wake(void) { woke = true; }

void frame_hold(uint32_t ms)
{
	hold_t = (time_ms() + ms) | 1;
}

void frame_draw(uint32_t now)
{
	if (hold_t) {
		if ((int32_t)(hold_t - now) > 0)
			return;
		hold_t = 0;
	}
	bool drawn = false;
	if (have_ui) {
		stack_update(now);
		if (stack_moving())
			power_activity(now);
		ui_tick();
		uint32_t t0 = time_us();
		struct backdrop b = { .screen = stack_screen() };
		drawn = flatmesh_mcu_draw(ui_modal() ? UI_PANEL_OVERLAY : b.screen, &b, display_lost());
		if (drawn) {
			draws++;
			draw_us += time_us() - t0;
		}
		frames++;
		woke = woke && !drawn;
	}
	if (!drawn) {
		uint32_t flags = irq_save();
		if (!sched_pending())
			wfi();
		irq_restore(flags);
	}
}

void frame_ambient(void)
{
	struct backdrop b = { .screen = UI_PANEL_FACE, .ambient = true };
	flatmesh_mcu_draw(UI_PANEL_FACE, &b, display_lost());
}

void frame_log(void)
{
	if (frames)
		log_printf("ui: %lu of %lu passes drew, %lu us per draw\n", draws, frames, draws ? draw_us / draws : 0);
	frames = draws = draw_us = 0;
}
uint32_t frame_waited_us(void) { return 0; }
