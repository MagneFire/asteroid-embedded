/* The RM69090 AMOLED panel on SPI, with a data/command line: a command
 * byte with the line low, its parameters with it high, the chip select
 * held across both. There is no frame buffer: the UI hands over lines of
 * RGB565 as it renders them (ui_draw_*), they are packed big-endian into
 * one of two chunk buffers, and DMA sends one while the other fills. A
 * picture starts with its window and a memory write, each further chunk
 * with a memory write continue, after the panel's tearing signal. The
 * panel takes its brightness as a command. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"

#define DCS_POWER_MODE	0x0a
#define DCS_SLEEP_IN	0x10
#define DCS_SLEEP_OUT	0x11
#define DCS_DISPLAY_OFF	0x28
#define DCS_DISPLAY_ON	0x29
#define DCS_COLUMNS	0x2a
#define DCS_ROWS	0x2b
#define DCS_WRITE	0x2c
#define DCS_TEAR_ON	0x35
#define DCS_IDLE_OFF	0x38
#define DCS_IDLE_ON	0x39
#define DCS_PIXELS	0x3a
#define DCS_BRIGHTNESS	0x51
#define PIXELS_RGB565	0x75
#define MODE_SLEEP_OUT	BIT(4)
#define MODE_DISPLAY_ON	BIT(2)
#define CHUNK		4092u
#define TE_WAIT_MS	25

#define BUS		(board->panel.spi_bus)
#define CS		(board->panel.spi_cs)
#define DC		(board->panel.dc_gpio)

struct fb fb;

static uint8_t chunk[2][CHUNK] __attribute__((aligned(4)));
static unsigned fill, filled;
static bool started, panel_on, panel_asleep, idle, lost, lent;
static uint8_t level = 0xff;
static uint32_t frames, waits, timeouts, errors;
static uint16_t win_x, win_y, win_w, win_h, next_y;
static void (*idle_hook)(void);

void display_set_idle_hook(void (*fn)(void)) { idle_hook = fn; }
uint32_t display_timeouts(void) { return timeouts; }
uint32_t display_frames(void) { return frames; }
uint32_t display_waits(void) { return waits; }
uint32_t *display_backbuffer(void) { return NULL; }
void display_splash_size(uint32_t *w, uint32_t *h) { *w = *h = 0; }
void display_flip(void) {}

static int command(uint8_t cmd, const uint8_t *p, unsigned n)
{
	gpio_set(DC, 0);
	int r = iom_spi_write(BUS, CS, &cmd, 1, n != 0);
	gpio_set(DC, 1);
	if (!r && n)
		r = iom_spi_write(BUS, CS, p, n, false);
	if (r)
		errors++;
	return r;
}

/* A command's reply, a byte at a time after it on the same select. */
static int query(uint8_t cmd, uint8_t *p, unsigned n)
{
	gpio_set(DC, 0);
	int r = iom_spi_write(BUS, CS, &cmd, 1, true);
	gpio_set(DC, 1);
	return r ? r : iom_spi_read(BUS, CS, p, n, false);
}

int dsi_dcs_write(uint8_t cmd, const uint8_t *params, unsigned n)
{
	if (!panel_on)
		return -1;
	iom_spi_wait(BUS);
	return command(cmd, params, n);
}

void dsi_low_power(bool on) {}

static void run(const struct dcs_step *s, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		command(s[i].cmd, s[i].data, s[i].n);
		if (s[i].delay_ms)
			delay_ms(s[i].delay_ms);
	}
}

static void power(bool on)
{
	for (unsigned i = 0; i < ARRAY_SIZE(board->panel.enable_gpio); i++)
		if (board->panel.enable_gpio[i]) {
			gpio_set(board->panel.enable_gpio[i], on);
			gpio_config(board->panel.enable_gpio[i], 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
		}
}

static bool start(void)
{
	gpio_set(DC, 0);
	gpio_config(DC, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 4);
	gpio_set(board->panel.reset_gpio, 0);
	gpio_config(board->panel.reset_gpio, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_config(board->panel.te_gpio, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	power(true);
	delay_ms(12);
	gpio_set(board->panel.reset_gpio, 1);
	delay_ms(12);
	run(board->panel.on, board->panel.on_n);
	command(DCS_PIXELS, (const uint8_t[]){ PIXELS_RGB565 }, 1);
	command(DCS_BRIGHTNESS, &level, 1);
	panel_on = true;
	idle = false;
	lost = true;
	return true;
}

static void stop(void)
{
	iom_spi_wait(BUS);
	command(DCS_DISPLAY_OFF, NULL, 0);
	command(DCS_SLEEP_IN, NULL, 0);
	delay_ms(125);
	gpio_set(board->panel.reset_gpio, 0);
	power(false);
	panel_on = false;
}

/* The AP's display driver can power the panel down under us, on a
 * charger event: one no longer out of sleep with its display on starts
 * over. Only trusted once a read has been seen to work. */
static bool reads_work;

static bool mode_has(uint8_t bits)
{
	uint8_t mode;
	iom_spi_wait(BUS);
	return !query(DCS_POWER_MODE, &mode, 1) && mode != 0xff && (mode & bits) == bits;
}

/* The ID bytes, for telling the panels apart; 0 for any not read. */
void display_read_id(uint8_t id[3])
{
	for (unsigned i = 0; i < 3; i++)
		if (lent || query((uint8_t)(0xda + i), &id[i], 1))
			id[i] = 0;
}

static void let_go(unsigned pad)
{
	if (pad)
		gpio_config(pad, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
}

static void let_go_lines(void)
{
	iom_stop(BUS);
	let_go(DC);
	let_go(board->panel.te_gpio);
	let_go(board->panel.reset_gpio);
	for (unsigned i = 0; i < ARRAY_SIZE(board->panel.spi_pads); i++)
		let_go(board->panel.spi_pads[i]);
}

/* At the start the enables are let go of rather than driven low: the
 * AP's panel is on. */
static void let_go_all(void)
{
	let_go_lines();
	for (unsigned i = 0; i < ARRAY_SIZE(board->panel.enable_gpio); i++)
		let_go(board->panel.enable_gpio[i]);
}

void display_start_lent(void) { lent = true; }

static void window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

bool display_init(void)
{
	if (!board->panel.spi_hz || (!lent && iom_spi_init(BUS, board->panel.spi_hz, board->panel.spi_mode))) {
		log_printf("display: no SPI panel\n");
		return false;
	}
	fb.width = board->panel.width;
	fb.height = fb.panel_height = board->panel.height;
	fb.stride = 0;
	fb.buf[0] = fb.buf[1] = NULL;
	if (lent) {
		let_go_all();
		started = panel_asleep = true;
		log_printf("display: %lux%lu on spi%u, left to the AP\n", fb.width, fb.height, BUS);
		return true;
	}
	uint32_t t0 = time_ms();
	start();
	uint8_t id[3];
	display_read_id(id);
	reads_work = mode_has(MODE_SLEEP_OUT);
	started = true;
	panel_asleep = false;
	log_printf("display: %lux%lu on spi%u at %lu Hz, id %02x %02x %02x, reads %s, up in %lu ms\n", fb.width, fb.height,
		   BUS, board->panel.spi_hz, id[0], id[1], id[2], reads_work ? "work" : "fail", time_ms() - t0);
	return true;
}

/* The tearing line goes high as the panel starts its blanking. */
static void wait_te(void)
{
	uint32_t t0 = time_ms();
	while (gpio_get(board->panel.te_gpio))
		if (time_ms() - t0 > TE_WAIT_MS)
			goto late;
	while (!gpio_get(board->panel.te_gpio)) {
		if (time_ms() - t0 > TE_WAIT_MS)
			goto late;
		if (idle_hook)
			idle_hook();
	}
	waits++;
	return;
late:
	timeouts++;
}

static void window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	uint16_t x0 = x + board->panel.col_offset, x1 = x0 + w - 1, y1 = y + h - 1;
	command(DCS_COLUMNS, (const uint8_t[]){ x0 >> 8, x0 & 0xff, x1 >> 8, x1 & 0xff }, 4);
	command(DCS_ROWS, (const uint8_t[]){ y >> 8, y & 0xff, y1 >> 8, y1 & 0xff }, 4);
}

static bool open_window, first_chunk;

/* A window is one memory write: its command, then every chunk with the
 * select held to the last, so the panel takes it as one stream. A chunk
 * goes out when the next pixel does not fit, so more always follows
 * it. */
static void send(bool more)
{
	if (!filled)
		return;
	iom_spi_wait(BUS);
	if (first_chunk) {
		gpio_set(DC, 0);
		uint8_t cmd = DCS_WRITE;
		iom_spi_write(BUS, CS, &cmd, 1, true);
		gpio_set(DC, 1);
	}
	if (iom_spi_write_start(BUS, CS, chunk[fill], filled, more))
		errors++;
	first_chunk = false;
	fill ^= 1;
	filled = 0;
}

/* From the UI: what it is about to draw. Lines outside the box, or not
 * the next one down in it, start a box of their own. */
void ui_draw_begin(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	if (!panel_on || panel_asleep) {
		lost = true;
		return;
	}
	iom_spi_wait(BUS);
	wait_te();
	open_window = false;
	filled = 0;
	fill = 0;
}

static void begin_run(uint16_t x, uint16_t y, uint16_t w)
{
	send(false);
	iom_spi_wait(BUS);
	win_x = x;
	win_y = next_y = y;
	win_w = w;
	win_h = (uint16_t)(fb.height - y);
	window(win_x, win_y, win_w, win_h);
	first_chunk = open_window = true;
}

static inline uint32_t swap16(uint32_t v)
{
	return (v & 0x00ff00ffu) << 8 | (v >> 8 & 0x00ff00ffu);
}

void ui_draw_line(uint32_t y, uint32_t x, uint32_t w, const uint16_t *px)
{
	if (!panel_on || panel_asleep || !w || x + w > fb.width || y >= fb.height)
		return;
	if (!open_window || x != win_x || w != win_w || y != next_y)
		begin_run((uint16_t)x, (uint16_t)y, (uint16_t)w);
	/* The panel takes each pixel high byte first: two at a time are
	 * swapped in one word. */
	for (uint32_t i = 0; i < w;) {
		if (filled + 2 > CHUNK)
			send(true);
		uint8_t *buf = chunk[fill];
		uint32_t f = filled, n = (CHUNK - f) / 2;
		if (n > w - i)
			n = w - i;
		uint32_t end = i + n;
		if (!(f & 3)) {
			/* px is only two-byte aligned; the M4 loads it a word
			 * at a time all the same. */
			for (; i + 3 < end; i += 4, f += 8) {
				uint32_t a, b;
				__builtin_memcpy(&a, px + i, 4);
				__builtin_memcpy(&b, px + i + 2, 4);
				*(uint32_t *)(buf + f) = swap16(a);
				*(uint32_t *)(buf + f + 4) = swap16(b);
			}
			for (; i + 1 < end; i += 2, f += 4) {
				uint32_t v;
				__builtin_memcpy(&v, px + i, 4);
				*(uint32_t *)(buf + f) = swap16(v);
			}
		}
		for (; i < end; i++, f += 2) {
			buf[f] = (uint8_t)(px[i] >> 8);
			buf[f + 1] = (uint8_t)px[i];
		}
		filled = f;
	}
	next_y = (uint16_t)(y + 1);
}

void ui_draw_end(void)
{
	if (!panel_on || panel_asleep)
		return;
	send(false);
	frames++;
}

void display_wait_idle(void) { iom_spi_wait(BUS); }

bool display_lost(void)
{
	if (!panel_on || panel_asleep || !lost)
		return false;
	lost = false;
	return true;
}

void display_set_brightness(uint8_t value)
{
	level = value;
	if (panel_on && !panel_asleep) {
		iom_spi_wait(BUS);
		command(DCS_BRIGHTNESS, &level, 1);
	}
}

void display_set_backlight_code(uint8_t code)
{
	if (panel_on && !panel_asleep) {
		iom_spi_wait(BUS);
		command(DCS_BRIGHTNESS, &code, 1);
	}
}

void display_sleep(void)
{
	if (panel_on)
		stop();
	panel_asleep = true;
}

/* On, but dark until display_show, so a new picture is there first. */
void display_wake_hidden(void)
{
	if (panel_on && !lent && reads_work && !mode_has(MODE_SLEEP_OUT | MODE_DISPLAY_ON)) {
		log_printf("display: panel was turned off, starting it again\n");
		panel_on = false;
	}
	if (!panel_on && !lent) {
		uint8_t keep = level;
		level = 0;
		start();
		level = keep;
	}
	panel_asleep = false;
}

void display_show(void)
{
	if (!panel_on)
		return;
	iom_spi_wait(BUS);
	command(DCS_BRIGHTNESS, &level, 1);
	command(DCS_DISPLAY_ON, NULL, 0);
}

void display_wake(void)
{
	display_wake_hidden();
	display_show();
}

/* Idle mode: eight colours, scanned more slowly, for the always-on
 * face. */
void display_idle(bool on)
{
	if (!panel_on || on == idle)
		return;
	iom_spi_wait(BUS);
	command(on ? DCS_IDLE_ON : DCS_IDLE_OFF, NULL, 0);
	idle = on;
}

bool display_rest(void)
{
	iom_spi_wait(BUS);
	return true;
}

void display_active(void) {}

/* As the vendor's firmware hands the panel back: the AP's pixel format,
 * and C4 00 against the C4 80 of the start. */
static const struct dcs_step handback[] = {
	{ DCS_PIXELS, 1, 0, { 0x77 } },
	{ DCS_TEAR_ON, 1, 0, { 0x00 } },
	{ 0xc4, 1, 0, { 0x00 } },
};

/* Idle mode and the last window are ours, not the vendor's: undone
 * first. */
void display_lend(bool to_ap)
{
	if (to_ap == lent)
		return;
	lent = to_ap;
	if (to_ap) {
		iom_spi_wait(BUS);
		if (panel_on) {
			command(DCS_IDLE_OFF, NULL, 0);
			idle = false;
			window(0, 0, (uint16_t)fb.width, (uint16_t)fb.height);
			run(handback, ARRAY_SIZE(handback));
		}
		power(false);
		let_go_lines();
		panel_on = false;
		panel_asleep = true;
		return;
	}
	for (const struct board_mcu_pad *p = board->mcu_pads; p && p->func != 0xff; p++)
		for (unsigned i = 0; i < ARRAY_SIZE(board->panel.spi_pads); i++)
			if (p->pad == board->panel.spi_pads[i])
				pad_set(p);
	iom_spi_init(BUS, board->panel.spi_hz, board->panel.spi_mode);
}

/* No frame buffer to draw the console into. */
void fbcon_init(void) {}
void fbcon_draw(uint32_t *buf) {}
void fbcon_set_overlay(bool on) {}
void fbcon_puts(const char *s) {}
void fbcon_printf(const char *fmt, ...) {}
