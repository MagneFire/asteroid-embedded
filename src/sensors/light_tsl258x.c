/* beluga's light sensor, a TSL258x on I2C with the touch controller and
 * powered all the time. The vendor's firmware runs it at 111x and 100 ms
 * while the screen is on and clears its power bit otherwise; here it is
 * powered only while kept (light_keep), so only while awake, or while
 * the AP has it on (tsl258x_share).
 *
 * Kept on, the chip converts one integration after another by itself
 * and light_finish reads the last one, both channels: two short
 * transfers and no waiting, as the bus is the touch controller's too.
 * The ADC goes on at the first start or finish POWER_MS after the power
 * bit, and a reading counts once a whole integration has run since. A
 * reading that needs the other gain gives none and the next one uses
 * it.
 *
 * The vendor's firmware leaves the chip alone unless its ID register
 * says TSL258x; so does this driver, and the ID goes to the log. It is
 * asked the first time the chip is wanted, as the AP may have the bus
 * at the start, and again while it does not answer.
 *
 * The lux takes off what the screen adds, from the colour drawn over
 * the sensor and the panel's brightness, with the watch's calibration
 * from the vendor's factory record in flash, only ever read. Only the
 * watch's own picture is known: while the AP has the panel, or it is
 * dark or showing the always-on face, nothing is taken off. */
#include <string.h>
#include "asteroid.h"
#include "bt/bt.h"
#include "companion/sensors.h"
#include "display/display.h"
#include "sensors/tsl258x.h"
#include "power.h"
#include "ui/ui.h"

#define BUS		(board->light_pins.i2c.bus)
#define ADDR		(board->light_pins.i2c.addr)
#define BUS_HZ		400000
#define PROFILE		(board->light_pins.profile)

#ifndef CAL_RECORD
#define CAL_RECORD	((const uint8_t *)(uintptr_t)(VENDOR_FACTORY_BASE + 0x50))
#endif

#define CMD_SELECT	0x80
#define CMD_AUTO_INC	0x20
#define CMD_SPECIAL	0x60
#define SPECIAL_INT_CLEAR 0x01

#define REG_CONTROL	0x00
#define REG_ITIME	0x01
#define REG_INTERRUPT	0x02
#define REG_ANALOG	0x07
#define REG_ID		0x12
#define REG_DATA0LOW	0x14

#define CONTROL_POWER	0x01
#define CONTROL_ADC_EN	0x02
#define CONTROL_VALID	0x10
#define ITIME_100MS	(256 - TSL258X_CYCLES)

static const uint8_t gain_codes[] = { 0x00, 0x03 };

/* The vendor's firmware waits 2 ms after the power bit. */
#define POWER_MS	3
#define INTEGRATION_MS	100
#define SLACK_MS	10
#define LOG_MS		5000

/* probed: its ID needs no asking again; ready: a TSL258x that took
 * its setup; lingering: the write that powers it down failed. */
static bool probed, ready, kept, lingering, settled = true, adc, own, shared, sweeping, have_reading, cal_factory;
static enum tsl258x_gain gain = TSL258X_GAIN_111X;
static uint32_t powered, valid_at, logged_at, misses;
static struct tsl258x_cal cal;
static struct tsl258x_reading last, last_logged;
/* The share of the model's screen light taken off, in per cent: for
 * trying out a correction on the watch. */
static uint32_t screen_pct = 100;

static int wr(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { CMD_SELECT | reg, val };
	return i2c_xfer(BUS, ADDR, b, 2, NULL, 0);
}

static int rd(uint8_t reg, uint8_t *v, unsigned n)
{
	uint8_t cmd = CMD_SELECT | reg;
	return i2c_xfer(BUS, ADDR, &cmd, 1, v, n);
}

static void say_cal(void (*say)(const char *fmt, ...))
{
	say("light: calibration %u %u %u %u, gain %u%%%s\n", cal.rgbw[0], cal.rgbw[1], cal.rgbw[2], cal.rgbw[3],
	    cal.gain_pct, cal_factory ? "" : ", the vendor's defaults");
}

/* The interrupt line is not used: off, and nothing left pending. */
static void probe(void)
{
	uint8_t id = 0, clear = CMD_SELECT | CMD_SPECIAL | SPECIAL_INT_CLEAR;
	int r = i2c_init(BUS, board->light_pins.i2c.sda, board->light_pins.i2c.scl, board->light_pins.i2c.func, BUS_HZ);
	if (!r)
		r = rd(REG_ID, &id, 1);
	if (r) {
		if (!misses++)
			log_printf("light: tsl258x not found (%d)\n", r);
		return;
	}
	if (!tsl258x_id_ok(id)) {
		probed = true;
		log_printf("light: id %02x is no tsl258x, left alone\n", id);
		return;
	}
	r = wr(REG_CONTROL, 0);
	if (!r)
		r = wr(REG_INTERRUPT, 0);
	if (!r)
		r = i2c_xfer(BUS, ADDR, &clear, 1, NULL, 0);
	probed = ready = !r;
	log_printf("light: tsl258x, id %02x%s\n", id, ready ? "" : ", no answer to its setup");
}

static bool tsl258x_init(void)
{
	uint8_t rec[TSL258X_CAL_SIZE];
	memcpy(rec, CAL_RECORD, sizeof rec);
	cal_factory = tsl258x_cal_read(&cal, rec);
	tsl258x_crop_set(PROFILE);
	say_cal(log_printf);
	return true;
}

static void say_reading(void (*say)(const char *fmt, ...), const struct tsl258x_reading *r)
{
	say("light: c0 %u c1 %u at %s, rgb %u %u %u, level %u, screen %ld, %ld lux\n", r->c0, r->c1,
	    r->gain == TSL258X_GAIN_1X ? "1x" : "111x", r->rgb[0], r->rgb[1], r->rgb[2], r->level, (long)r->screen,
	    (long)r->lux);
}

/* At most every LOG_MS, and only for something new. */
static void log_reading(uint32_t now)
{
	if (now - logged_at < LOG_MS)
		return;
	if (last.c0 == last_logged.c0 && last.c1 == last_logged.c1 && last.gain == last_logged.gain &&
	    last.level == last_logged.level && !memcmp(last.rgb, last_logged.rgb, sizeof last.rgb))
		return;
	say_reading(log_printf, &last);
	last_logged = last;
	logged_at = now;
}

static int32_t take(uint32_t now, uint16_t c0, uint16_t c1)
{
	struct tsl258x_reading *r = &last;
	r->c0 = c0;
	r->c1 = c1;
	r->gain = gain;
	r->c0_111x = gain == TSL258X_GAIN_1X ? (uint32_t)c0 * 111 : c0;
	r->c1_111x = gain == TSL258X_GAIN_1X ? (uint32_t)c1 * 111 : c1;
	bool lit = display_showing() && power_awake();
	if (lit)
		tsl258x_crop_rgb(r->rgb);
	else
		memset(r->rgb, 0, sizeof r->rgb);
	r->level = lit ? display_brightness() : 0;
	r->screen = tsl258x_screen_counts(PROFILE, &cal, r->rgb, r->level) * (float)screen_pct / 100;
	r->lux = tsl258x_lux(PROFILE, &cal, gain, c0, c1, r->screen, r->level);
	r->at_ms = now;
	have_reading = true;
	log_reading(now);
	return r->lux;
}

bool light_tsl258x_reading(struct tsl258x_reading *r)
{
	if (have_reading)
		*r = last;
	return have_reading;
}

static bool adc_on(uint32_t now)
{
	if (adc)
		return true;
	if (now - powered < POWER_MS || wr(REG_CONTROL, CONTROL_POWER | CONTROL_ADC_EN))
		return false;
	adc = true;
	valid_at = now + INTEGRATION_MS + SLACK_MS;
	return true;
}

static void tsl258x_start(void)
{
	if (kept)
		adc_on(time_ms());
}

static int32_t tsl258x_finish(void)
{
	uint32_t now = time_ms();
	uint8_t status = 0, b[4];
	if (!kept || !adc_on(now) || (int32_t)(now - valid_at) < 0)
		return -1;
	if (rd(REG_CONTROL, &status, 1) || !(status & CONTROL_VALID) || rd(REG_DATA0LOW | CMD_AUTO_INC, b, sizeof b))
		return -1;
	uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
	enum tsl258x_gain next = tsl258x_next_gain(gain, c0);
	if (next == gain)
		return take(now, c0, c1);
	/* The integration under way mixes both gains. */
	if (!wr(REG_ANALOG, gain_codes[next])) {
		gain = next;
		valid_at = now + 2 * INTEGRATION_MS + SLACK_MS;
	}
	return -1;
}

/* Whether the chip is powered as wanted; a write that failed is left
 * for the next call. */
static bool power_as_wanted(void)
{
	bool on = own || shared || sweeping;
	if (on == kept && !lingering)
		return true;
	if (!ready && on && !probed)
		probe();
	if (!ready)
		return !on || probed;
	adc = false;
	if (on) {
		powered = time_ms();
		kept = !wr(REG_CONTROL, CONTROL_POWER) && !wr(REG_ITIME, ITIME_100MS) && !wr(REG_ANALOG, gain_codes[gain]);
		if (kept) {
			lingering = false;
			return true;
		}
	}
	kept = false;
	lingering = wr(REG_CONTROL, 0) != 0;
	return !on && !lingering;
}

static void apply(void) { settled = power_as_wanted(); }

bool tsl258x_settled(void) { return settled; }

void tsl258x_lost(void)
{
	probed = ready = kept = lingering = adc = false;
	settled = !(own || shared || sweeping);
}

/* The levels a sweep steps through, and long enough at each for a whole
 * integration at that level. */
static const uint8_t sweep_levels[] = { 0, 51, 102, 153, 204, 255 };
#define SWEEP_MS	(2 * INTEGRATION_MS + 5 * SLACK_MS)

static uint16_t sweep_line[480];

/* The whole screen in one colour and the crop in another, straight to
 * the panel; the UI draws all of it again afterwards. */
static void paint(uint16_t rest, uint16_t inside)
{
	const uint16_t *c = PROFILE->crop;
	uint32_t w = fb.width < ARRAY_SIZE(sweep_line) ? fb.width : ARRAY_SIZE(sweep_line);
	ui_draw_begin(0, 0, w, fb.height);
	for (uint32_t y = 0; y < fb.height; y++) {
		bool in = y >= c[1] && y < c[3];
		for (uint32_t x = 0; x < w; x++)
			sweep_line[x] = in && x >= c[0] && x < c[2] ? inside : rest;
		ui_draw_line(y, 0, w, sweep_line);
	}
	ui_draw_end();
}

static bool sweep_read(uint16_t *c0, uint16_t *c1)
{
	uint8_t status = 0, b[4];
	wdt_feed();
	delay_ms(SWEEP_MS);
	if (rd(REG_CONTROL, &status, 1) || !(status & CONTROL_VALID) || rd(REG_DATA0LOW | CMD_AUTO_INC, b, sizeof b))
		return false;
	*c0 = (uint16_t)(b[0] | b[1] << 8);
	*c1 = (uint16_t)(b[2] | b[3] << 8);
	return true;
}

/* Holds the sensor for a sweep and has it converting, with the screen
 * on: false if it can not. */
static bool sweep_begin(void (*say)(const char *fmt, ...))
{
	if (!power_awake()) {
		say("light: the screen has to be on\n");
		return false;
	}
	/* A sweep holds up the main loop for seconds. */
	if (bt_host_connected() || sns_active()) {
		say("light: not while a phone is connected or the AP has sensors on\n");
		return false;
	}
	sweeping = true;
	apply();
	delay_ms(POWER_MS);
	if (kept && adc_on(time_ms()))
		return true;
	say("light: sensor not answering\n");
	sweeping = false;
	apply();
	return false;
}

static void sweep_end(uint8_t was, bool painted)
{
	if (painted)
		ui_mark_dirty(0, 0, fb.width, fb.height);
	display_set_brightness(was);
	sweeping = false;
	apply();
}

/* Channel 0 at each level with the screen as it is, black, white, or
 * black with a white crop, and the straight line through them: what the
 * screen adds per level step, and what is left at level 0. */
static void sweep(const char *what, void (*say)(const char *fmt, ...))
{
	bool black = !strcmp(what, "black"), white = !strcmp(what, "white"), crop = !strcmp(what, "crop");
	if (*what && !black && !white && !crop) {
		say("usage: light sweep [black|white|crop|grid [columns rows]]\n");
		return;
	}
	uint8_t was = display_brightness();
	if (!sweep_begin(say))
		return;
	if (black || white || crop)
		paint(white ? 0xffff : 0x0000, black ? 0x0000 : 0xffff);
	unsigned n = 0;
	float level[ARRAY_SIZE(sweep_levels)], counts[ARRAY_SIZE(sweep_levels)];
	int32_t scale = gain == TSL258X_GAIN_1X ? 111 : 1;
	for (unsigned i = 0; i < ARRAY_SIZE(sweep_levels); i++) {
		uint8_t l = sweep_levels[i], rgb[3];
		uint16_t c0, c1;
		display_set_brightness(l);
		if (!sweep_read(&c0, &c1)) {
			say("light: level %u, no reading\n", l);
			continue;
		}
		tsl258x_crop_rgb(rgb);
		say("light: level %u c0 %lu c1 %lu, rgb %u %u %u, model screen %ld\n", l, (unsigned long)c0 * scale,
		    (unsigned long)c1 * scale, rgb[0], rgb[1], rgb[2], (long)tsl258x_screen_counts(PROFILE, &cal, rgb, l));
		level[n] = l;
		counts[n++] = (float)c0 * scale;
	}
	float at0, per;
	if (tsl258x_fit(level, counts, n, &at0, &per))
		say("light: c0 = %ld + %ld per 100 levels, %ld at 255, %ld lux at level 0\n", (long)at0, (long)(per * 100),
		    (long)(per * 255), (long)tsl258x_lux(PROFILE, &cal, TSL258X_GAIN_111X, at0 <= 0 ? 0 : at0 >= 65535 ? 65535 : (uint16_t)at0, 0, 0, 0));
	sweep_end(was, black || white || crop);
}

#define GRID_MAX	16

/* A number and the spaces after it, false without digits. */
static bool number(const char **p, uint32_t *v)
{
	const char *q = *p;
	*v = 0;
	while (*q >= '0' && *q <= '9' && *v < 100000)
		*v = *v * 10 + (uint32_t)(*q++ - '0');
	if (q == *p)
		return false;
	while (*q == ' ')
		q++;
	*p = q;
	return true;
}

/* The panel takes windows of even start and size. */
static uint32_t grid_edge(uint32_t size, unsigned i, unsigned n)
{
	return i == n ? size : size * i / n & ~1u;
}

static void fill_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t colour)
{
	for (uint32_t i = 0; i < w && i < ARRAY_SIZE(sweep_line); i++)
		sweep_line[i] = colour;
	ui_draw_begin(x, y, w, h);
	for (uint32_t j = 0; j < h; j++)
		ui_draw_line(y + j, x, w, sweep_line);
	ui_draw_end();
}

/* At full brightness on a black screen, one white tile at a time: what
 * each adds to channel 0 over the black screen, row by row, against the
 * whole screen white, and the middle of what the sensor sees. */
static void sweep_grid(const char *args, void (*say)(const char *fmt, ...))
{
	uint32_t cols = 6, rows = 7;
	if (*args && (!number(&args, &cols) || !number(&args, &rows) || *args || !cols || !rows || cols > GRID_MAX ||
		      rows > GRID_MAX)) {
		say("usage: light sweep grid [columns rows], up to %u each\n", GRID_MAX);
		return;
	}
	uint8_t was = display_brightness();
	if (!sweep_begin(say))
		return;
	uint32_t w = fb.width < ARRAY_SIZE(sweep_line) ? fb.width : ARRAY_SIZE(sweep_line);
	int32_t scale = gain == TSL258X_GAIN_1X ? 111 : 1;
	uint16_t c0 = 0, c1;
	display_set_brightness(255);
	fill_box(0, 0, w, fb.height, 0xffff);
	int32_t full = sweep_read(&c0, &c1) ? c0 * scale : -1;
	fill_box(0, 0, w, fb.height, 0x0000);
	int32_t dark = sweep_read(&c0, &c1) ? c0 * scale : -1;
	say("light: grid %lux%lu at level 255, black %ld, white %ld\n", cols, rows, dark, full);
	float sum = 0, sx = 0, sy = 0;
	for (uint32_t r = 0; r < rows && dark >= 0; r++) {
		uint32_t y0 = grid_edge(fb.height, r, rows), y1 = grid_edge(fb.height, r + 1, rows);
		char line[GRID_MAX * 7 + 1];
		unsigned n = 0;
		for (uint32_t c = 0; c < cols; c++) {
			uint32_t x0 = grid_edge(w, c, cols), x1 = grid_edge(w, c + 1, cols);
			fill_box(x0, y0, x1 - x0, y1 - y0, 0xffff);
			int32_t d = sweep_read(&c0, &c1) ? c0 * scale - dark : -9999;
			fill_box(x0, y0, x1 - x0, y1 - y0, 0x0000);
			n += (unsigned)snprintf(line + n, sizeof line - n, " %6ld", (long)d);
			if (d > 0) {
				sum += d;
				sx += d * (x0 + x1) / 2.0f;
				sy += d * (y0 + y1) / 2.0f;
			}
		}
		say("light: y %3lu-%3lu%s\n", y0, y1 - 1, line);
	}
	if (sum > 0)
		say("light: tiles add %ld of %ld, centre at x %ld y %ld\n", (long)sum, (long)(full - dark), (long)(sx / sum),
		    (long)(sy / sum));
	sweep_end(was, true);
}

static void tsl258x_command(const char *args, void (*say)(const char *fmt, ...))
{
	if (!strncmp(args, "sweep", 5) && (!args[5] || args[5] == ' ')) {
		const char *what = args + 5;
		while (*what == ' ')
			what++;
		if (!strncmp(what, "grid", 4) && (!what[4] || what[4] == ' ')) {
			what += 4;
			while (*what == ' ')
				what++;
			sweep_grid(what, say);
		} else {
			sweep(what, say);
		}
		return;
	}
	if (!strncmp(args, "screen ", 7)) {
		uint32_t pct;
		const char *p = args + 7;
		if (!number(&p, &pct) || *p || pct > 1000) {
			say("usage: light screen <0-1000 percent>\n");
			return;
		}
		screen_pct = pct;
	} else if (*args) {
		say("usage: light [sweep [black|white|crop|grid [columns rows]] | screen <percent>]\n");
		return;
	}
	say_cal(say);
	say("light: screen light taken off at %lu%%\n", screen_pct);
	if (!have_reading) {
		say("light: no reading yet\n");
		return;
	}
	say_reading(say, &last);
	say("light: %lu ms ago\n", time_ms() - last.at_ms);
}

static void tsl258x_keep(bool on)
{
	own = on;
	apply();
}

void tsl258x_share(bool on)
{
	shared = on;
	apply();
}

const struct light_driver light_tsl258x = {
	.init = tsl258x_init,
	.start = tsl258x_start,
	.finish = tsl258x_finish,
	.keep = tsl258x_keep,
	.command = tsl258x_command,
};
