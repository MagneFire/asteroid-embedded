/* The first image to run on the watch, loaded into SRAM by `swdtool
 * ramrun`. It has no way to write flash. It logs the clock and the
 * timer, starts the panel with the board's pads and fills it with
 * colour bars that change every 2 s, and logs the keys and touches as
 * they change. The motor buzzes once at the start and on each press of
 * the upper key.
 * The application processor must have let go of the panel first. */
#include "asteroid.h"
#include "gitrev.h"
#include "display/display.h"
#include "ui/ui.h"
#include "input/touch.h"

int flash_erase_page(uint32_t addr) { return -1; }
int flash_program(uint32_t addr, const uint32_t *words, uint32_t n) { return -1; }

static const uint16_t colours[] = { 0xf800, 0x07e0, 0x001f, 0xffff, 0xffe0, 0x07ff, 0xf81f, 0x7bef };

/* A square where the driver puts the finger, to see it under the finger. */
static void dot(int x, int y)
{
	static uint16_t line[12];
	for (unsigned i = 0; i < 12; i++)
		line[i] = 0x0000;
	int x0 = x - 6 < 0 ? 0 : x - 6 > (int)fb.width - 12 ? (int)fb.width - 12 : x - 6;
	int y0 = y - 6 < 0 ? 0 : y - 6 > (int)fb.height - 12 ? (int)fb.height - 12 : y - 6;
	/* The panel takes windows on even columns and rows only. */
	x0 &= ~1;
	y0 &= ~1;
	ui_draw_begin((uint32_t)x0, (uint32_t)y0, 12, 12);
	for (int r = 0; r < 12; r++)
		ui_draw_line((uint32_t)(y0 + r), (uint32_t)x0, 12, line);
	ui_draw_end();
	display_wait_idle();
}

static void bars(unsigned shift)
{
	static uint16_t line[480];
	uint32_t w = fb.width < 480 ? fb.width : 480;
	for (uint32_t x = 0; x < w; x++)
		line[x] = colours[(x * 8 / w + shift) % 8];
	ui_draw_begin(0, 0, w, fb.height);
	for (uint32_t y = 0; y < fb.height; y++)
		ui_draw_line(y, 0, w, line);
	ui_draw_end();
	display_wait_idle();
}

int main(int argc, char **argv)
{
	soc_init();
	irq_enable();
	log_init();
	log_printf("ramtest: %s\n", GITREV);
	cpu_clock_log();
	board_detect(0, NULL);
	for (const struct board_mcu_pad *p = board->mcu_pads; p && p->func != 0xff; p++)
		pad_set(p);
	power_key_irq_init();
	bool motor = board->motor && board->motor->init();
	bool touch = board->touch && board->touch->init();
	log_printf("ramtest: motor %s, touch %s\n", motor ? "up" : "missing", touch ? "up" : "missing");
	bool shown = display_init();
	if (shown) {
		display_set_brightness(60);
		bars(0);
		display_show();
		log_printf("ramtest: bars drawn, frames %lu, te waits %lu, te timeouts %lu\n", display_frames(),
			   display_waits(), display_timeouts());
	}
	unsigned keys = ~0u, shift = 0;
	uint32_t next = time_ms() + 2000, buzz_end = 0;
	bool buzzing = false, touched = false;
	if (motor) {
		board->motor->set(true);
		buzzing = true;
		buzz_end = time_ms() + 150;
	}
	for (;;) {
		struct touch t;
		static int x0, x1, y0, y1;
		if (touch && board->touch->poll(&t)) {
			if (t.event == TOUCH_DOWN)
				x0 = x1 = t.x, y0 = y1 = t.y;
			x0 = t.x < x0 ? t.x : x0, x1 = t.x > x1 ? t.x : x1;
			y0 = t.y < y0 ? t.y : y0, y1 = t.y > y1 ? t.y : y1;
			if (shown && t.down) {
				dot(t.x, t.y);
				touched = true;
			}
			if (t.event == TOUCH_DOWN)
				log_printf("ramtest: touch down at %d,%d\n", t.x, t.y);
			else if (t.event == TOUCH_UP)
				log_printf("ramtest: touch up at %d,%d, x %d..%d, y %d..%d\n", t.x, t.y, x0, x1, y0, y1);
		}
		if (buzzing && (int32_t)(time_ms() - buzz_end) >= 0) {
			board->motor->set(false);
			buzzing = false;
		}
		unsigned now = gpio_get(board->key_gpio[0]) | gpio_get(board->key_gpio[1]) << 1;
		if (now != keys) {
			log_printf("ramtest: %lu ms, upper key %u, lower key %u\n", time_ms(), now & 1, now >> 1);
			if (motor && keys != ~0u && !(now & 1) && (keys & 1)) {
				board->motor->set(true);
				buzzing = true;
				buzz_end = time_ms() + 150;
			}
			keys = now;
		}
		if ((int32_t)(time_ms() - next) >= 0) {
			uint32_t t0 = time_ms();
			if (shown && !touched)
				bars(++shift);
			if (shift % 5 == 0)
				log_printf("ramtest: %lu ms, frame in %lu ms, frames %lu, te timeouts %lu\n", time_ms(),
					   time_ms() - t0, display_frames(), display_timeouts());
			next += 2000;
		}
	}
}
