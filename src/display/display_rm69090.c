/* The RM69090 AMOLED panel on SPI, with a data/command line: a command
 * byte with the line low, its parameters with it high, the chip select
 * held across both. There is no frame buffer: the UI hands over lines of
 * RGB565 as it renders them (ui_draw_*), they are packed big-endian into
 * one of two chunk buffers, and DMA sends one while the other fills. A
 * picture starts with its window and a memory write, each further chunk
 * with a memory write continue, after the panel's tearing signal. The
 * panel takes its brightness as a command. The light sensor under the
 * panel sees each line too, for the screen's own light. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "display/rm69090_id.h"
#include "sensors/tsl258x.h"
#include "detect.h"

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
/* Reads take a clock of at least 300 ns with its data up to 120 ns after
 * the falling edge; at 48 MHz the rising edge samples the bit before.
 * 0 reads at the write rate. */
#define READ_HZ		1500000u

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
static enum rm69090_framing framing;
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

/* A command's one-byte reply, after it on the same select. */
static int query(uint8_t cmd, uint8_t raw[2])
{
	if (READ_HZ)
		iom_spi_set_hz(BUS, READ_HZ, board->panel.spi_mode);
	gpio_set(DC, 0);
	int r = iom_spi_write(BUS, CS, &cmd, 1, true);
	gpio_set(DC, 1);
	if (!r)
		r = iom_spi_read(BUS, CS, raw, 2, false);
	if (READ_HZ)
		iom_spi_set_hz(BUS, board->panel.spi_hz, board->panel.spi_mode);
	return r;
}

static int query_byte(uint8_t cmd, uint8_t *v)
{
	uint8_t raw[2];
	int r = query(cmd, raw);
	*v = rm69090_byte(raw, framing);
	return r;
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

static void power_on(void)
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
}

static void init_table(void)
{
	run(board->panel.on, board->panel.on_n);
	command(DCS_PIXELS, (const uint8_t[]){ PIXELS_RGB565 }, 1);
	command(DCS_BRIGHTNESS, &level, 1);
	panel_on = true;
	idle = false;
	lost = true;
}

static void start(void)
{
	power_on();
	init_table();
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
	return !query_byte(DCS_POWER_MODE, &mode) && mode != 0xff && (mode & bits) == bits;
}

/* The replies to DA, DB and DC, for telling the panels apart; 0 for any
 * not read. */
static void read_id(uint8_t raw[3][2])
{
	for (unsigned i = 0; i < 3; i++)
		if (query((uint8_t)(0xda + i), raw[i]))
			raw[i][0] = raw[i][1] = 0;
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

/* Pad 12 is the 46 mm panel's second enable and the 41 mm touch supply.
 * The 46 mm table, used until the ID is read, drives it high, as stock
 * does on both. */
static bool start_detect(uint8_t raw[3][2])
{
	power_on();
	read_id(raw);
	const struct board *was = board;
	bool asleep_read = board_by_panel_id(raw, &framing);
	if (!asleep_read) {
		/* A panel that answers only out of sleep: the 46 mm table,
		 * dark, and the ID once more. */
		uint8_t keep = level;
		level = 0;
		init_table();
		level = keep;
		read_id(raw);
	}
	framing = board_detect_panel(raw);
	if (asleep_read)
		init_table();
	else if (board != was)
		start();
	else
		command(DCS_BRIGHTNESS, &level, 1);
	return asleep_read;
}

/* Lent at the start with no size kept, the size is read once the panel
 * is the watch's. The UI has the size of the 46 mm watch by then, so a
 * 41 mm panel needs a boot of its own, which now starts with the
 * panel. */
static bool unread;

static void start_unread(void)
{
	uint8_t raw[3][2];
	const struct board *was = board;
	unread = false;
	start_detect(raw);
	if (board != was) {
		log_printf("display: the panel is %s's, starting again for its size\n", board->name);
		reboot();
	}
}

bool display_init(void)
{
	if (!board->panel.spi_hz || (!lent && iom_spi_init(BUS, board->panel.spi_hz, board->panel.spi_mode))) {
		log_printf("display: no SPI panel\n");
		return false;
	}
	fb.stride = 0;
	fb.buf[0] = fb.buf[1] = NULL;
	if (lent) {
		unread = !board_detect_kept();
		fb.width = board->panel.width;
		fb.height = fb.panel_height = board->panel.height;
		let_go_all();
		started = panel_asleep = true;
		log_printf("display: %lux%lu on spi%u, left to the AP\n", fb.width, fb.height, BUS);
		return true;
	}
	uint32_t t0 = time_ms();
	uint8_t raw[3][2];
	bool asleep_read = start_detect(raw);
	fb.width = board->panel.width;
	fb.height = fb.panel_height = board->panel.height;
	reads_work = mode_has(MODE_SLEEP_OUT);
	started = true;
	panel_asleep = false;
	log_printf("display: %lux%lu on spi%u at %lu Hz, id read %02x%02x %02x%02x %02x%02x at %lu Hz %s, reads %s, "
		   "up in %lu ms\n",
		   fb.width, fb.height, BUS, board->panel.spi_hz, raw[0][0], raw[0][1], raw[1][0], raw[1][1], raw[2][0],
		   raw[2][1], READ_HZ ? READ_HZ : board->panel.spi_hz, asleep_read ? "in sleep" : "after the 46 mm table",
		   reads_work ? "work" : "fail", time_ms() - t0);
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

static inline __attribute__((always_inline)) uint32_t swap16(uint32_t v)
{
	return (v & 0x00ff00ffu) << 8 | (v >> 8 & 0x00ff00ffu);
}

typedef uint32_t __attribute__((may_alias)) word;

/* The panel takes each pixel high byte first: two at a time are swapped
 * in one word, eight a pass by load and store multiple where the line
 * is word aligned (GCC does not use them here). buf is at least two-byte
 * aligned. */
static __attribute__((noinline)) void pack(uint8_t *buf, const uint16_t *px, uint32_t n)
{
	if ((uintptr_t)buf & 2) {
		*(uint16_t *)buf = (uint16_t)swap16(*px++);
		buf += 2;
		n--;
	}
	word *d = (word *)buf;
#ifdef __thumb2__
	if (!((uintptr_t)px & 2) && n >= 8) {
		uint32_t k = n / 8;
		n %= 8;
		__asm__ volatile("1:	ldmia	%[s]!, {r4, r5, r6, r7}\n\t"
				 "rev16	r4, r4\n\t"
				 "rev16	r5, r5\n\t"
				 "rev16	r6, r6\n\t"
				 "rev16	r7, r7\n\t"
				 "stmia	%[d]!, {r4, r5, r6, r7}\n\t"
				 "subs	%[k], %[k], #1\n\t"
				 "bne	1b"
				 : [s] "+r"(px), [d] "+r"(d), [k] "+r"(k)
				 :
				 : "r4", "r5", "r6", "r7", "cc", "memory");
	}
#endif
	for (; n >= 2; n -= 2, px += 2) {
		uint32_t v;
		__builtin_memcpy(&v, px, 4);
		*d++ = swap16(v);
	}
	if (n)
		*(uint16_t *)d = (uint16_t)swap16(*px);
}

void ui_draw_line(uint32_t y, uint32_t x, uint32_t w, const uint16_t *px)
{
	if (!panel_on || panel_asleep || !w || x + w > fb.width || y >= fb.height)
		return;
	if (!open_window || x != win_x || w != win_w || y != next_y)
		begin_run((uint16_t)x, (uint16_t)y, (uint16_t)w);
	tsl258x_crop_line(y, x, w, px);
	for (uint32_t i = 0; i < w;) {
		if (filled + 2 > CHUNK)
			send(true);
		uint32_t n = (CHUNK - filled) / 2;
		if (n > w - i)
			n = w - i;
		pack(chunk[fill] + filled, px + i, n);
		filled += n * 2;
		i += n;
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

uint8_t display_brightness(void) { return level; }

bool display_showing(void) { return panel_on && !panel_asleep && !lent; }

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
		if (unread)
			start_unread();
		else
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
