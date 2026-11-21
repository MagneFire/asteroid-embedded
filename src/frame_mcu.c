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

static bool have_ui, woke, drawn;
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
	drawn = false;
	if (hold_t) {
		if ((int32_t)(hold_t - now) > 0)
			return;
		hold_t = 0;
	}
	if (have_ui) {
		stack_update(now);
		if (stack_moving())
			power_activity(now);
		ui_tick();
		uint32_t t0 = time_us();
		bool modal = ui_modal();
		struct backdrop b = { .screen = stack_screen(), .modal = modal, .popup = modal && ui_popup(),
				      .edges = stack_edges(), .pending = stack_waiting() };
		b.page = stack_page(&b.pages, &b.next);
		drawn = flatmesh_mcu_draw(modal ? UI_PANEL_OVERLAY : stack_panel(), &b, display_lost());
		if (drawn) {
			draws++;
			draw_us += time_us() - t0;
		}
		frames++;
		woke = woke && !drawn;
	}
}

/* A moving screen or a running animation goes on at the 1 ms tick. */
uint32_t frame_rest_ms(void)
{
	if (drawn)
		return 0;
	uint32_t now = time_ms();
	if (hold_t)
		return (int32_t)(hold_t - now) > 0 ? hold_t - now : 1;
	if (!have_ui)
		return UINT32_MAX;
	if (stack_animating(now))
		return 1;
	uint32_t next = ui_next_ms();
	return next ? next : 1;
}

void frame_ambient(int dx, int dy)
{
	struct backdrop b = { .screen = UI_PANEL_FACE, .ambient = true, .pending = stack_waiting(), .dx = dx, .dy = dy };
	flatmesh_mcu_draw(UI_PANEL_FACE, &b, display_lost());
}

void frame_log(void)
{
	if (frames)
		log_printf("ui: %lu of %lu passes drew, %lu us per draw\n", draws, frames, draws ? draw_us / draws : 0);
	frames = draws = draw_us = 0;
}
uint32_t frame_waited_us(void) { return 0; }
