/* The Moto 360's display: the SoC sends frames in DSI command mode to
 * an SSD2848 bridge, which keeps them in a memory of 320 x 320 and
 * scans an OTM3201 panel from it by itself, 45 times a second. The
 * panel shows 290 of the lines; the frame the UI draws is the full
 * round picture, of which only those are sent. Commands for the bridge
 * are generic writes of a 16-bit register and a 32-bit value; the
 * panel's own are passed on by the bridge while its retransmission is
 * switched on. Nothing of the bootloader's setup is relied on: power,
 * resets, the bridge's and the panel's registers are all set here, in
 * the vendor driver's order and with its values.
 *
 * With the screen off everything is off, the bridge by cutting its own
 * power, which only its reset line undoes. With the always-on face
 * shown the bridge keeps scanning the panel at 30 Hz while the SoC's
 * side rests with the lanes in their low power state. */
#include <string.h>
#include "asteroid.h"
#include "display.h"
#include "soc/omap3/dss.h"

#define WIDTH			320
#define PANEL_HEIGHT		290
#define BRIDGE_HEIGHT		320

#define DCS_SLEEP_IN		0x10
#define DCS_SLEEP_OUT		0x11
#define DCS_DISPLAY_OFF		0x28
#define DCS_DISPLAY_ON		0x29
#define DCS_COLUMNS		0x2a
#define DCS_ROWS		0x2b
#define DCS_MEMORY_START	0x2c
#define DCS_MEMORY_CONTINUE	0x3c
#define DCS_TEAR_OFF		0x34
#define DCS_TEAR_ON		0x35
#define DCS_POWER_MODE		0x0a
#define POWER_MODE_NORMAL	0x1c
#define BRIDGE_RETRANSMIT	0xff
#define PANEL_REVISION		0xb0

#define BRIDGE_RESET_MS		20
#define RESET_RELEASE_MS	10
#define CLOCK_SETTLE_MS		6
#define FRAME_TIMEOUT_US	250000
#define INIT_TRIES		3

/* What a command of the tables is: for the bridge (B, a register and
 * its value), a DCS command for the bridge (D), one for the panel
 * through the bridge (P), a wait in ms (W). */
enum { END, B, D, P, W };

/* The bridge: its PLL at 26 MHz * 50 / 6 = 216.7 MHz, the panel's
 * pixel clock and timing (392 x 334 in all around 320 x 320, 45 Hz),
 * tearing signal from its own scan, then its DSI output to the panel
 * in burst video mode. The panel: unlocked, its timing, inversion and
 * gamma, then locked again. */
static const uint8_t init_bridge[] = {
	B, 6, 0x00, 0x08, 0x01, 0xf4, 0x05, 0x32,
	B, 6, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x0b,
	B, 6, 0x00, 0x14, 0x0c, 0x07, 0x80, 0x0f,
	D, 1, DCS_SLEEP_OUT,
	W, 1, 1,
	B, 6, 0x10, 0x08, 0x01, 0x20, 0x01, 0x45,
	B, 6, 0x20, 0x0c, 0x00, 0x00, 0x00, 0x02,
	B, 6, 0x20, 0x10, 0x00, 0x5f, 0x00, 0x1f,
	B, 6, 0x20, 0x14, 0x01, 0x88, 0x00, 0x34,
	B, 6, 0x20, 0x18, 0x01, 0x4e, 0x00, 0x0c,
	B, 6, 0x20, 0x20, 0x01, 0x40, 0x01, 0x40,
	B, 6, 0x20, 0x24, 0x01, 0x40, 0x01, 0x40,
	B, 6, 0x20, 0x30, 0x00, 0x00, 0x00, 0x15,
	B, 6, 0x20, 0x34, 0x00, 0x00, 0x00, 0x00,
	B, 6, 0x20, 0x38, 0x01, 0x3f, 0x01, 0x3f,
	B, 6, 0x20, 0x3c, 0x01, 0x40, 0x01, 0x40,
	B, 6, 0x20, 0xa0, 0x00, 0x00, 0x05, 0x00,
	D, 5, DCS_COLUMNS, 0x00, 0x00, 0x01, 0x3f,
	D, 5, DCS_ROWS, 0x00, 0x00, 0x01, 0x3f,
	B, 6, 0x60, 0x08, 0x00, 0x04, 0x00, 0x0a,
	B, 6, 0x60, 0x0c, 0x0c, 0x34, 0x02, 0x0a,
	B, 6, 0x60, 0x10, 0x01, 0x40, 0x02, 0x14,
	B, 6, 0x60, 0x14, 0x01, 0x00, 0x01, 0x4e,
	B, 6, 0x60, 0x40, 0x13, 0x01, 0x0a, 0x01,
	B, 6, 0x60, 0x44, 0x05, 0x05, 0x04, 0x0a,
	B, 6, 0x60, 0x84, 0x00, 0x00, 0x01, 0x40,
	END,
};

#define GAMMA	0x00, 0x06, 0x17, 0x11, 0x16, 0x25, 0x0e, 0x0c, 0x0c, 0x0e, 0x0c, 0x2f, 0x07, 0x0a, 0x3f, 0x3f, 0x3f
static const uint8_t init_panel[] = {
	P, 3, 0xf0, 0x54, 0x47,
	P, 2, 0xa0, 0x00,
	P, 4, 0xbd, 0x00, 0x11, 0x31,
	P, 2, 0xe9, 0x46,
	P, 2, 0xb1, 0x12,
	P, 2, 0xe2, 0xf5,
	P, 5, 0xba, 0x06, 0x15, 0x2b, 0x01,
	P, 6, 0xb3, 0x02, 0x0a, 0x14, 0x2a, 0x2a,
	P, 5, 0xb5, 0x78, 0x78, 0x76, 0xf6,
	P, 18, 0xc0, GAMMA,
	P, 18, 0xc1, GAMMA,
	P, 18, 0xc2, GAMMA,
	P, 18, 0xc3, GAMMA,
	P, 18, 0xc4, GAMMA,
	P, 18, 0xc5, GAMMA,
	P, 2, 0xa0, 0x80,
	P, 3, 0xf0, 0x00, 0x00,
	P, 1, DCS_SLEEP_OUT,
	END,
};

/* The pixel clock as a fraction of the bridge's 9 MHz: for 60 or 45
 * scans a second, as the board asks, and 30 for the always-on face. */
static const uint8_t rate_60[] = { 0x20, 0x10, 0x00, 0xf7, 0x00, 0x6c };
static const uint8_t rate_45[] = { 0x20, 0x10, 0x00, 0x5f, 0x00, 0x1f };
static const uint8_t rate_30[] = { 0x20, 0x10, 0x00, 0xef, 0x00, 0x34 };

static const uint8_t *rate_awake(void)
{
	return board->panel.hz >= 60 ? rate_60 : rate_45;
}
/* The bridge cuts its own power. */
static const uint8_t power_cut[] = { 0x10, 0x28, 0x00, 0x00, 0x00, 0x01 };

struct fb fb;
static bool panel_on, panel_asleep = true, resting, kicked, use_te = true;
static unsigned te_misses;
static uint32_t frames, waits, timeouts;
static uint8_t backlight_level = 255, backlight_code;
static void (*idle_hook)(void);

void display_set_idle_hook(void (*fn)(void)) { idle_hook = fn; }
uint32_t display_timeouts(void) { return timeouts; }
uint32_t display_frames(void) { return frames; }
uint32_t display_waits(void) { return waits; }
uint32_t *display_backbuffer(void) { return fb.buf[fb.back]; }
void display_splash_size(uint32_t *w, uint32_t *h) { *w = *h = 0; }

static int retransmit(bool on)
{
	return dsi_write(true, (const uint8_t[]){ BRIDGE_RETRANSMIT, on }, 2, true);
}

static int panel_write(const uint8_t *d, unsigned n, bool sync)
{
	int r = retransmit(true);
	if (!r)
		r = dsi_write(false, d, n, sync);
	if (!r)
		r = retransmit(false);
	return r;
}

static int run(const uint8_t *t)
{
	for (; t[0] != END; t += 2 + t[1]) {
		int r = 0;
		switch (t[0]) {
		case B: r = dsi_write(true, t + 2, t[1], true); break;
		case D: r = dsi_write(false, t + 2, t[1], true); break;
		case P: r = panel_write(t + 2, t[1], true); break;
		case W: delay_ms(t[2]); break;
		}
		if (r) {
			log_printf("display: command %02x %02x failed (%d)\n", t[2], t[1] > 1 ? t[3] : 0, r);
			return r;
		}
	}
	return 0;
}

/* Every register of the bridge read back: it takes writes it has not
 * understood without a word. One that reads differently is only logged,
 * as the vendor's driver carries on then too; no answer is a failure. */
static bool bridge_verified(void)
{
	for (const uint8_t *t = init_bridge; t[0] != END; t += 2 + t[1]) {
		uint8_t got[4];
		if (t[0] != B)
			continue;
		int n = dsi_read(true, t + 2, got, 4);
		if (n != 4) {
			log_printf("display: bridge register %02x%02x not read (%d)\n", t[2], t[3], n);
			return false;
		}
		if (memcmp(got, t + 4, 4))
			log_printf("display: bridge register %02x%02x reads %02x%02x%02x%02x\n", t[2], t[3], got[0], got[1], got[2],
				   got[3]);
	}
	return true;
}

static int window(unsigned y0, unsigned y1)
{
	int r = dsi_write(false, (const uint8_t[]){ DCS_COLUMNS, 0, 0, (WIDTH - 1) >> 8, (WIDTH - 1) & 0xff }, 5, false);
	if (!r)
		r = dsi_write(false, (const uint8_t[]){ DCS_ROWS, (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 }, 5,
			      true);
	return r;
}

/* The bridge's memory set to black: it holds noise after power-up,
 * which would show until the first frame, and the lines below the
 * panel's, which no frame reaches, are scanned out all the same. */
static void clear_memory(unsigned first_line)
{
	uint8_t packet[1 + 120];
	memset(packet, 0, sizeof packet);
	packet[0] = DCS_MEMORY_START;
	dsi_high_speed(true);
	if (!window(first_line, BRIDGE_HEIGHT - 1))
		for (unsigned left = WIDTH * (BRIDGE_HEIGHT - first_line) * 3; left; packet[0] = DCS_MEMORY_CONTINUE) {
			unsigned n = left < 120 ? left : 120;
			if (dsi_write(false, packet, n + 1, false))
				break;
			left -= n;
		}
	dsi_sync();
	dsi_high_speed(false);
}

static void supplies(bool on)
{
	const struct board *b = board;
	if (!on) {
		gpio_set(b->panel.reset_gpio, 0);
		gpio_set(b->panel.bridge_reset, 0);
		delay_ms(BRIDGE_RESET_MS);
		periph_clock(false);
		gpio_set(b->panel.mem_gpio, 0);
		gpio_set(b->panel.vio_gpio, 1);
		gpio_set(b->panel.bridge_supply, 0);
		regulator_off(b->panel.io_ldo);
		return;
	}
	gpio_set(b->panel.reset_gpio, 0);
	gpio_set(b->panel.bridge_reset, 0);
	gpio_config(b->panel.reset_gpio, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_config(b->panel.bridge_reset, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_config(b->panel.mem_gpio, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_set(b->panel.bridge_supply, 1);
	gpio_config(b->panel.bridge_supply, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	if (regulator_on(b->panel.io_ldo, b->panel.io_uv))
		log_printf("display: panel supply not on\n");
	/* The clock first and a wait: what leaks from it charges the 1.8 V
	 * side, which otherwise dips when its switch closes and resets
	 * the sensor hub. The switch (low is on) is then closed slowly,
	 * through the pad's pull-down, before the pin drives it. */
	periph_clock(true);
	delay_ms(CLOCK_SETTLE_MS);
	pad_config(b->panel.vio_pad, PAD_MODE_SAFE | PAD_PULL_ENABLE);
	delay_us(300);
	gpio_set(b->panel.vio_gpio, 0);
	gpio_config(b->panel.vio_gpio, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	pad_config(b->panel.vio_pad, PAD_MODE_GPIO);
	gpio_set(b->panel.mem_gpio, 1);
}

/* The lines of a panel the bootloader left on, taken as they are. */
static void supplies_keep(void)
{
	const struct board *b = board;
	static const uint8_t high[] = { offsetof(struct board, panel.reset_gpio), offsetof(struct board, panel.bridge_reset),
					offsetof(struct board, panel.bridge_supply), offsetof(struct board, panel.mem_gpio) };
	for (unsigned i = 0; i < ARRAY_SIZE(high); i++) {
		unsigned pin = *((const uint8_t *)b + high[i]);
		gpio_set(pin, 1);
		gpio_config(pin, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	}
	regulator_on(b->panel.io_ldo, b->panel.io_uv);
	periph_clock(true);
	gpio_set(b->panel.vio_gpio, 0);
	gpio_config(b->panel.vio_gpio, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	pad_config(b->panel.vio_pad, PAD_MODE_GPIO);
}

static void reset_pulse(void)
{
	gpio_set(board->panel.reset_gpio, 0);
	gpio_set(board->panel.bridge_reset, 0);
	delay_ms(BRIDGE_RESET_MS);
	gpio_set(board->panel.reset_gpio, 1);
	gpio_set(board->panel.bridge_reset, 1);
	delay_ms(RESET_RELEASE_MS);
}

static bool panel_answers(void)
{
	uint8_t mode = 0, rev = 0;
	if (retransmit(true))
		return false;
	int n = dsi_read(false, (const uint8_t[]){ DCS_POWER_MODE }, &mode, 1);
	dsi_read(false, (const uint8_t[]){ PANEL_REVISION }, &rev, 1);
	retransmit(false);
	log_printf("display: panel power mode %02x (%d), revision %02x\n", mode, n, rev);
	return n == 1 && mode == POWER_MODE_NORMAL;
}

/* From everything off to a picture: the link in low power mode for the
 * setup, then the tearing signal, the picture on and high speed. With
 * keep the bridge and the panel are as the bootloader set them up,
 * showing its picture from the bridge's memory, and only the link to
 * them is made anew; if they do not answer as they should they are
 * started from off after all. */
static bool start(bool keep)
{
	if (keep)
		supplies_keep();
	else
		supplies(true);
	for (unsigned attempt = 1; attempt <= INIT_TRIES; attempt++) {
		if (!dss_on(WIDTH, PANEL_HEIGHT, false))
			break;
		bool ok;
		if (keep) {
			ok = !dsi_sync() && panel_answers();
			if (ok) {
				clear_memory(PANEL_HEIGHT);
				log_printf("display: the bootloader's picture is kept\n");
			} else {
				log_printf("display: the bootloader's picture is not kept\n");
				keep = false;
				dss_off(false);
				supplies(true);
				continue;
			}
		} else {
			reset_pulse();
			ok = !run(init_bridge) && bridge_verified() && !run(init_panel);
			if (ok) {
				/* The panel wants 120 ms after waking; the memory
				 * is cleared in that time. */
				uint32_t t0 = time_ms();
				clear_memory(0);
				while (time_ms() - t0 < 120)
					wdt_feed();
				ok = !panel_write((const uint8_t[]){ DCS_DISPLAY_ON }, 1, true);
			}
			if (ok && !panel_answers())
				log_printf("display: the panel does not report itself on\n");
		}
		if (ok)
			ok = !dsi_write(false, (const uint8_t[]){ DCS_TEAR_ON, 0 }, 2, true) &&
			     !dsi_write(false, (const uint8_t[]){ DCS_DISPLAY_ON }, 1, true);
		if (ok)
			ok = !dsi_write(true, rate_awake(), sizeof rate_30, true);
		if (ok) {
			dsi_high_speed(true);
			panel_on = true;
			resting = kicked = false;
			return true;
		}
		log_printf("display: start-up failed, attempt %u\n", attempt);
		dss_off(false);
	}
	supplies(false);
	return false;
}

static void frame_wait(void)
{
	if (!kicked)
		return;
	uint32_t t0 = time_us();
	bool done;
	while (!(done = dss_frame_done())) {
		if (idle_hook)
			idle_hook();
		if (time_us() - t0 > FRAME_TIMEOUT_US)
			break;
	}
	waits += time_us() - t0;
	kicked = false;
	if (done) {
		te_misses = 0;
		return;
	}
	timeouts++;
	dss_frame_abort();
	/* Without the panel's tearing signal frames go out as they come. */
	if (use_te && ++te_misses == 3) {
		log_printf("display: no tearing signal from the panel, sending frames without it\n");
		use_te = false;
	}
}

static void stop(void)
{
	frame_wait();
	dsi_high_speed(false);
	panel_write((const uint8_t[]){ DCS_DISPLAY_OFF }, 1, true);
	panel_write((const uint8_t[]){ DCS_SLEEP_IN }, 1, false);
	dsi_write(false, (const uint8_t[]){ DCS_DISPLAY_OFF }, 1, true);
	delay_ms(50);
	dsi_write(false, (const uint8_t[]){ DCS_SLEEP_IN }, 1, false);
	delay_ms(20);
	dsi_write(true, power_cut, sizeof power_cut, false);
	delay_ms(5);
	dss_off(false);
	supplies(false);
	panel_on = false;
}

bool display_init(void)
{
	fb.width = WIDTH;
	fb.panel_height = PANEL_HEIGHT;
	fb.height = board->panel.round ? WIDTH : PANEL_HEIGHT;
	fb.stride = WIDTH;
	fb.buf[0] = (uint32_t *)(uintptr_t)FB_BASE;
	fb.buf[1] = (uint32_t *)(uintptr_t)(FB_BASE + fb.stride * fb.height * 4);
	fb.back = 0;
	memset(fb.buf[0], 0, fb.stride * fb.height * 8);
	if (board->backlight && !board->backlight->init())
		log_printf("display: no backlight\n");
	uint32_t t0 = time_ms();
	if (!start(boot_panel_lit())) {
		log_printf("display: not started\n");
		return false;
	}
	panel_asleep = false;
	log_printf("display: %lux%lu of %lu lines, up in %lu ms\n", fb.width, fb.panel_height, fb.height, time_ms() - t0);
	return true;
}

/* While the always-on face is shown, between frames, the SoC's side of
 * the link rests: the lanes in their low power state and the display
 * subsystem off, the bridge scanning the panel on its own. */
bool display_rest(void)
{
	if (resting || !panel_on)
		return true;
	frame_wait();
	dsi_write(false, (const uint8_t[]){ DCS_TEAR_OFF }, 1, true);
	dss_off(true);
	resting = true;
	return true;
}

void display_active(void)
{
	if (!resting || !panel_on)
		return;
	if (!dss_on(WIDTH, PANEL_HEIGHT, true))
		reboot_fatal("display: link stays down");
	dsi_high_speed(true);
	dsi_write(false, (const uint8_t[]){ DCS_TEAR_ON, 0 }, 2, true);
	resting = false;
}

void display_flip(void)
{
	if (!panel_on)
		return;
	display_active();
	uint32_t *buf = fb.buf[fb.back];
	dcache_clean_range(buf, fb.stride * fb.panel_height * 4);
	frame_wait();
	if (window(0, PANEL_HEIGHT - 1) || !dss_frame_start(buf, use_te)) {
		timeouts++;
		return;
	}
	kicked = true;
	frames++;
	fb.back ^= 1;
}

void display_wait_idle(void) { frame_wait(); }

/* The backlight goes dark with the panel and comes back with it, so
 * the level is kept for display_show. */
void display_set_brightness(uint8_t level)
{
	backlight_level = level;
	backlight_code = 0;
	if (board->backlight && !panel_asleep)
		board->backlight->set(level);
}

void display_set_backlight_code(uint8_t code)
{
	backlight_code = code;
	if (board->backlight && !panel_asleep)
		board->backlight->set_code(code);
}

void display_sleep(void)
{
	if (board->backlight)
		board->backlight->set(0);
	backlight_code = 0;
	if (panel_on) {
		display_active();
		stop();
	}
	panel_asleep = true;
}

/* On with the backlight still off, so a new frame can be drawn before
 * display_show lights it. */
void display_wake_hidden(void)
{
	if (!panel_on && !start(false))
		reboot_fatal("display: does not start");
	panel_asleep = false;
}

void display_show(void)
{
	if (!board->backlight)
		return;
	if (backlight_code)
		board->backlight->set_code(backlight_code);
	else
		board->backlight->set(backlight_level);
}

void display_wake(void)
{
	display_wake_hidden();
	display_show();
}

/* The always-on face is scanned out more slowly. */
void display_idle(bool on)
{
	if (!panel_on)
		return;
	display_active();
	frame_wait();
	dsi_write(true, on ? rate_30 : rate_awake(), sizeof rate_30, true);
}

/* For the console: a DCS command to the bridge. */
int dsi_dcs_write(uint8_t cmd, const uint8_t *params, unsigned n)
{
	uint8_t d[16];
	if (!panel_on || n + 1 > sizeof d)
		return -1;
	display_active();
	frame_wait();
	d[0] = cmd;
	memcpy(d + 1, params, n);
	return dsi_write(false, d, n + 1, true);
}

void dsi_low_power(bool on) { dsi_high_speed(!on); }
