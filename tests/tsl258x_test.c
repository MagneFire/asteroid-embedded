/* beluga's TSL258x: its lux, the screen's light, the gain and its
 * driver. The expected lux are worked out by hand from the vendor's
 * formula (Sol's report), not by this code. */
#include <stdio.h>
#include <string.h>

/* The saved watch's factory record: 244 748 163 74 and 1.0953. */
static uint8_t cal_record[20] = { 244, 0, 0, 0, 0xec, 2, 0, 0, 163, 0, 0, 0, 74, 0, 0, 0, 0x95, 0x32, 0x8c, 0x3f };
#define CAL_RECORD cal_record
#include "sensors/light_tsl258x.c"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const struct board test_board = {
	.light_pins = { .i2c = { .bus = 0, .sda = 6, .scl = 5, .addr = 0x29 }, .profile = &tsl258x_46mm },
};
const struct board *board = &test_board;

/* The chip: its registers, channel 0 at each gain (channel 1 a tenth
 * of it), and whether a whole integration has run since the ADC went
 * on. */
static uint8_t regs[0x20], id = 0x94;
static uint16_t counts[2];
static bool absent, cleared;
static unsigned transfers;
static uint32_t now;
static int fail_after = -1;

static uint8_t brightness;

uint32_t time_ms(void) { return now; }
void log_printf(const char *fmt, ...) {}
uint8_t display_brightness(void) { return brightness; }
int i2c_init(unsigned bus, unsigned sda, unsigned scl, unsigned func, uint32_t bus_hz)
{
	CHECK(bus == 0 && sda == 6 && scl == 5);
	return 0;
}

static void integrate(void)
{
	if ((regs[REG_CONTROL] & (CONTROL_POWER | CONTROL_ADC_EN)) != (CONTROL_POWER | CONTROL_ADC_EN))
		return;
	unsigned g = regs[REG_ANALOG] == 0x03;
	CHECK(regs[REG_ANALOG] == 0x00 || g);
	CHECK(regs[REG_ITIME] == 0xdb);
	regs[REG_DATA0LOW] = (uint8_t)counts[g];
	regs[REG_DATA0LOW + 1] = (uint8_t)(counts[g] >> 8);
	regs[REG_DATA0LOW + 2] = (uint8_t)(counts[g] / 10);
	regs[REG_DATA0LOW + 3] = (uint8_t)(counts[g] / 10 >> 8);
	regs[REG_CONTROL] |= CONTROL_VALID;
}

int i2c_xfer(unsigned bus, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	CHECK(bus == 0 && addr == 0x29 && wn && (w[0] & CMD_SELECT));
	transfers++;
	if (absent || !fail_after)
		return -6;
	if (fail_after > 0)
		fail_after--;
	if ((w[0] & 0x60) == CMD_SPECIAL) {
		CHECK(w[0] == 0xe1 && wn == 1 && !rn);
		cleared = true;
		return 0;
	}
	uint8_t reg = w[0] & 0x1f;
	if (wn == 2) {
		/* The valid bit lasts while the ADC stays on. */
		uint8_t valid = reg == REG_CONTROL && regs[reg] & w[1] & CONTROL_ADC_EN ? regs[reg] & CONTROL_VALID : 0;
		regs[reg] = w[1] | valid;
		return 0;
	}
	if (reg == REG_DATA0LOW)
		CHECK(w[0] & CMD_AUTO_INC);
	for (unsigned i = 0; i < rn; i++)
		r[i] = regs[reg + i];
	if (reg == REG_ID)
		*r = id;
	return 0;
}

static const struct light_driver *d = &light_tsl258x;

/* One of power.c's readings: a start, and the finish 60 ms later, with
 * the chip converting in between. */
static int32_t reading(void)
{
	d->start();
	now += 60;
	integrate();
	int32_t lux = d->finish();
	now += 190;
	return lux;
}

static void driver_test(void)
{
	/* Nothing at 0x29: no reading, and keeping it on does nothing. */
	absent = true;
	CHECK(!d->init());
	absent = false;
	d->keep(true);
	CHECK(!kept && d->finish() == -1);

	/* Something else at 0x29 is only asked for its ID. */
	id = 0x24;
	transfers = 0;
	CHECK(!d->init());
	CHECK(transfers == 1 && !cleared);
	d->keep(true);
	CHECK(!kept && transfers == 1);

	/* Found: powered off, its interrupt off and cleared, the
	 * calibration from the record. */
	id = 0x94;
	regs[REG_CONTROL] = CONTROL_POWER;
	regs[REG_INTERRUPT] = 0x12;
	struct tsl258x_reading r;
	CHECK(!light_tsl258x_reading(&r));
	CHECK(d->init());
	CHECK(!regs[REG_CONTROL] && !regs[REG_INTERRUPT] && cleared);
	CHECK(cal_factory && cal.rgbw[1] == 748 && cal.gain_pct == 109);

	/* Not kept, starts and finishes leave the bus alone. */
	transfers = 0;
	d->start();
	CHECK(d->finish() == -1);
	CHECK(transfers == 0);

	/* Kept: powered at 111x and 100 ms, the ADC on from the first
	 * finish POWER_MS after, and the first reading once a whole
	 * integration has run. */
	counts[1] = 4500;
	counts[0] = 40;
	now = 1000;
	d->keep(true);
	CHECK(regs[REG_CONTROL] == CONTROL_POWER && regs[REG_ITIME] == 0xdb && regs[REG_ANALOG] == 0x03);
	d->start();
	CHECK(!(regs[REG_CONTROL] & CONTROL_ADC_EN));
	CHECK(reading() == -1);
	CHECK(regs[REG_CONTROL] & CONTROL_ADC_EN);
	CHECK(reading() == 1509);
	CHECK(light_tsl258x_reading(&r) && r.lux == 1509 && r.c0 == 4500 && r.c1 == 450 && r.c0_111x == 4500);

	/* Each later reading is two transfers and no waiting. */
	transfers = 0;
	uint32_t t = now;
	d->start();
	CHECK(d->finish() == 1509);
	CHECK(transfers == 2 && now == t);

	/* A white screen over the sensor at full brightness adds 1047
	 * counts. */
	uint16_t white[402];
	for (unsigned i = 0; i < 402; i++)
		white[i] = 0xffff;
	for (unsigned y = 255; y < 305; y++)
		tsl258x_crop_line(y, 0, 402, white);
	brightness = 255;
	CHECK(reading() == 843);
	CHECK(light_tsl258x_reading(&r) && r.rgb[0] == 255 && r.level == 255 && (int32_t)r.screen == 1047);
	brightness = 0;
	CHECK(reading() == 1509);

	/* Too bright for 111x: no reading, 1x from then on, and none until
	 * a whole integration at 1x has run. */
	counts[1] = 30000;
	CHECK(reading() == -1);
	CHECK(regs[REG_ANALOG] == 0x00 && gain == TSL258X_GAIN_1X);
	counts[0] = 400;
	counts[1] = 0xffff;
	transfers = 0;
	d->start();
	CHECK(d->finish() == -1 && transfers == 0);
	now += 250;
	CHECK(reading() == 14897);
	CHECK(light_tsl258x_reading(&r) && r.gain == TSL258X_GAIN_1X && r.c0_111x == 44400 && r.c1_111x == 4440);

	/* Dim at 1x goes back to 111x. */
	counts[0] = 199;
	counts[1] = 22000;
	CHECK(reading() == -1 && gain == TSL258X_GAIN_111X);
	now += 250;
	CHECK(reading() == 7381);

	/* No valid bit, no reading. */
	regs[REG_CONTROL] &= (uint8_t)~CONTROL_VALID;
	d->start();
	CHECK(d->finish() == -1);

	/* A failed transfer is no reading, and the next one works. */
	fail_after = 1;
	now += 250;
	integrate();
	CHECK(d->finish() == -1);
	fail_after = -1;
	CHECK(reading() == 7381);

	/* Letting go powers it down; keeping it again starts over. */
	d->keep(false);
	CHECK(!regs[REG_CONTROL]);
	transfers = 0;
	CHECK(reading() == -1 && transfers == 0);
	d->keep(true);
	CHECK(reading() == -1);
	CHECK(reading() == 7381);
	d->keep(true);
	CHECK(reading() == 7381);

	/* A chip that stops answering is not kept. */
	d->keep(false);
	fail_after = 0;
	d->keep(true);
	CHECK(!kept && !regs[REG_CONTROL]);
	fail_after = -1;
	CHECK(reading() == -1);
}

static const struct tsl258x_profile *const p46 = &tsl258x_46mm, *const p41 = &tsl258x_41mm;
static const uint8_t black[3], white_rgb[3] = { 255, 255, 255 }, red[3] = { 255 }, green[3] = { 0, 255 },
			    blue[3] = { 0, 0, 255 };

static bool near(float a, float b)
{
	return a - b < 0.01f && b - a < 0.01f;
}

/* The lux for counts at 111x with the screen showing rgb at level. */
static int32_t lux_at(const struct tsl258x_profile *p, const struct tsl258x_cal *c, uint16_t c0, uint16_t c1,
		      const uint8_t rgb[3], uint8_t level)
{
	return tsl258x_lux(p, c, TSL258X_GAIN_111X, c0, c1, tsl258x_screen_counts(p, c, rgb, level), level);
}

static void formula_test(void)
{
	struct tsl258x_cal c, def;
	uint8_t rec[20];

	/* The record as saved from the watch; its gain in whole per cent. */
	CHECK(tsl258x_cal_read(&c, cal_record));
	CHECK(c.rgbw[0] == 244 && c.rgbw[1] == 748 && c.rgbw[2] == 163 && c.rgbw[3] == 74 && c.gain_pct == 109);

	/* Erased, out of range or a gain that is no number: the vendor's
	 * defaults. */
	memset(rec, 0xff, sizeof rec);
	CHECK(!tsl258x_cal_read(&def, rec));
	CHECK(def.rgbw[0] == 216 && def.rgbw[1] == 611 && def.rgbw[2] == 128 && def.rgbw[3] == 257 && def.gain_pct == 100);
	memcpy(rec, cal_record, sizeof rec);
	rec[12] = 1;
	rec[13] = 0;
	CHECK(!tsl258x_cal_read(&def, rec) && def.rgbw[3] == 257);
	rec[12] = 0x88;
	rec[13] = 0x13;
	CHECK(!tsl258x_cal_read(&def, rec));
	rec[12] = 0x87;
	CHECK(tsl258x_cal_read(&def, rec) && def.rgbw[3] == 4999);
	memcpy(rec, cal_record, sizeof rec);
	memcpy(rec + 16, (const uint8_t[]){ 0, 0, 0xa0, 0x41 }, 4);
	CHECK(tsl258x_cal_read(&def, rec) && def.gain_pct == 2000);
	memcpy(rec + 16, (const uint8_t[]){ 0, 0, 0xc8, 0x41 }, 4);
	CHECK(!tsl258x_cal_read(&def, rec));
	memcpy(rec + 16, (const uint8_t[]){ 0xcd, 0xcc, 0x4c, 0x3d }, 4);
	CHECK(!tsl258x_cal_read(&def, rec));
	memcpy(rec + 16, (const uint8_t[]){ 0, 0, 0xc0, 0x7f }, 4);
	CHECK(!tsl258x_cal_read(&def, rec) && def.gain_pct == 100);

	/* Sol's example: 46 mm, (120, 90, 40) at level 80 is 43.525 counts;
	 * channel 1 picks the lux per count. */
	CHECK(near(tsl258x_screen_counts(p46, &c, (const uint8_t[]){ 120, 90, 40 }, 80), 43.525f));
	CHECK(lux_at(p46, &c, 6000, 700, (const uint8_t[]){ 120, 90, 40 }, 80) == 1998);
	CHECK(lux_at(p46, &c, 6000, 1500, (const uint8_t[]){ 120, 90, 40 }, 80) == 1068);
	/* The same with the vendor's default calibration. */
	CHECK(lux_at(p46, &def, 6000, 700, (const uint8_t[]){ 120, 90, 40 }, 80) == 1832);

	/* A black screen adds nothing at any level; up to 1000 counts the
	 * first lux per count, 0.3355 and 0.2559. */
	CHECK(tsl258x_screen_counts(p46, &c, black, 255) == 0 && tsl258x_screen_counts(p41, &c, black, 255) == 0);
	CHECK(lux_at(p46, &c, 0, 0, black, 0) == 0);
	CHECK(lux_at(p46, &c, 90, 9, black, 0) == 30);
	CHECK(lux_at(p46, &c, 1000, 0, black, 0) == 335);
	CHECK(lux_at(p46, &c, 1000, 1000, black, 0) == 335);
	CHECK(lux_at(p46, &c, 4500, 450, black, 0) == 1509);
	CHECK(lux_at(p41, &c, 90, 9, black, 0) == 23);
	CHECK(lux_at(p41, &c, 1000, 100, black, 0) == 255);
	CHECK(lux_at(p41, &c, 4500, 450, black, 0) == 1151);

	/* Above 1000 counts, channel 0 over channel 1: from 8, 4.5 and 3. */
	static const uint16_t c1s[] = { 0, 750, 751, 1333, 1334, 2000, 2001, 6000 };
	static const int32_t l46[] = { 2013, 2013, 1465, 1465, 1076, 1076, 757, 757 };
	static const int32_t l41[] = { 1535, 1535, 1190, 1190, 1165, 1165, 739, 739 };
	for (unsigned i = 0; i < 8; i++) {
		CHECK(lux_at(p46, &c, 6000, c1s[i], black, 0) == l46[i]);
		CHECK(lux_at(p41, &c, 6000, c1s[i], black, 0) == l41[i]);
	}

	/* White at full brightness on the 46 mm watch: 1047 counts, and
	 * what is left of 2000 is dim light. */
	CHECK(near(tsl258x_screen_counts(p46, &c, white_rgb, 255), 1047.010f));
	CHECK(near(tsl258x_screen_counts(p46, &c, white_rgb, 80), 328.474f));
	CHECK(lux_at(p46, &c, 2000, 100, white_rgb, 255) == 319);
	CHECK(lux_at(p46, &c, 3000, 1000, white_rgb, 255) == 246);
	CHECK(near(tsl258x_screen_counts(p46, &c, red, 255), 248.888f));
	CHECK(near(tsl258x_screen_counts(p46, &c, green, 30), 85.152f));
	CHECK(near(tsl258x_screen_counts(p46, &c, blue, 80), 52.047f));
	CHECK(lux_at(p46, &c, 800, 80, green, 128) == 146);

	/* The 41 mm watch's own curves for red and green, either side of
	 * level 60, and blue lit even at level 0. */
	CHECK(near(tsl258x_screen_counts(p41, &c, red, 80), 126.251f));
	CHECK(near(tsl258x_screen_counts(p41, &c, red, 30), 72.629f));
	CHECK(near(tsl258x_screen_counts(p41, &c, red, 59), 102.736f));
	CHECK(near(tsl258x_screen_counts(p41, &c, red, 60), 107.539f));
	CHECK(near(tsl258x_screen_counts(p41, &c, white_rgb, 255), 1063.783f));
	CHECK(near(tsl258x_screen_counts(p41, &c, green, 0), 0));
	CHECK(near(tsl258x_screen_counts(p41, &c, blue, 0), 9.813f));
	CHECK(lux_at(p41, &c, 3000, 200, red, 80) == 735);
	CHECK(lux_at(p41, &c, 3000, 200, red, 30) == 748);
	CHECK(lux_at(p41, &c, 800, 80, (const uint8_t[]){ 40, 200, 90 }, 128) == 118);

	/* At 1x the counts are 111 times as many; only a count full at its
	 * own gain skips the ratio. */
	CHECK(tsl258x_lux(p46, &c, TSL258X_GAIN_1X, 300, 30, 0, 0) == 11172);
	CHECK(tsl258x_lux(p41, &c, TSL258X_GAIN_1X, 300, 30, 0, 0) == 8519);
	CHECK(tsl258x_lux(p46, &c, TSL258X_GAIN_1X, 30000, 15000, 0, 0) == 420642);
	CHECK(tsl258x_lux(p46, &c, TSL258X_GAIN_1X, 40000, 20000, 0, 0) == 1489730);
	CHECK(lux_at(p46, &c, 37886, 20000, black, 0) == 12711);
	CHECK(lux_at(p46, &c, 37885, 20000, black, 0) == 4785);

	/* Nothing left near the brightest screen: a tenth of channel 0. */
	CHECK(lux_at(p46, &c, 300, 0, white_rgb, 255) == 30);
	CHECK(lux_at(p46, &c, 300, 0, white_rgb, 198) == 0);
}

static uint16_t line[402];

static void fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t colour)
{
	for (uint32_t i = 0; i < w; i++)
		line[i] = colour;
	for (uint32_t j = y; j < y + h; j++)
		tsl258x_crop_line(j, x, w, line);
}

static bool rgb_is(uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t rgb[3];
	tsl258x_crop_rgb(rgb);
	return rgb[0] == r && rgb[1] == g && rgb[2] == b;
}

static void crop_test(void)
{
	/* Nothing drawn yet is black. */
	tsl258x_crop_set(p46);
	CHECK(rgb_is(0, 0, 0));

	/* A whole white frame. */
	fill(0, 0, 402, 476, 0xffff);
	CHECK(rgb_is(255, 255, 255));

	/* A box over the left of the crop goes red: 9 of 17 sampled
	 * columns, averaged as light. */
	fill(0, 250, 45, 60, 0xf800);
	CHECK(rgb_is(255, 181, 181));

	/* Boxes beside it, rows between the sampled ones and a line
	 * just past the crop change nothing. */
	fill(100, 0, 302, 476, 0x0000);
	fill(0, 0, 402, 255, 0x0000);
	fill(0, 305, 402, 171, 0x0000);
	fill(0, 256, 402, 2, 0x0000);
	fill(0, 301, 402, 2, 0x0000);
	fill(69, 250, 31, 60, 0x0000);
	CHECK(rgb_is(255, 181, 181));

	/* The rest of the crop goes black: only the red left. */
	fill(45, 250, 24, 60, 0x0000);
	CHECK(rgb_is(192, 0, 0));

	/* A run starting between sampled columns. */
	fill(21, 255, 2, 50, 0xffff);
	CHECK(rgb_is(192, 0, 0));
	fill(21, 255, 3, 50, 0x001f);
	fill(24, 255, 45, 50, 0x001f);
	fill(0, 255, 21, 50, 0x001f);
	CHECK(rgb_is(0, 0, 255));

	/* A uniform colour gives its own value back. */
	fill(0, 255, 402, 50, 16 << 11 | 32 << 5 | 16);
	CHECK(rgb_is(132, 130, 132));

	/* The 41 mm crop: 12 by 10 samples, half its rows white. */
	tsl258x_crop_set(p41);
	CHECK(rgb_is(0, 0, 0));
	fill(0, 5, 320, 15, 0xffff);
	CHECK(rgb_is(187, 187, 187));
	fill(140, 20, 60, 15, 0xffff);
	CHECK(rgb_is(255, 255, 255));
	fill(184, 0, 136, 360, 0x0000);
	CHECK(rgb_is(255, 255, 255));
}

int main(void)
{
	/* A TSL258x is 0x9n. */
	CHECK(tsl258x_id_ok(0x90) && tsl258x_id_ok(0x94) && tsl258x_id_ok(0x9f));
	CHECK(!tsl258x_id_ok(0x80) && !tsl258x_id_ok(0xa0) && !tsl258x_id_ok(0x00) && !tsl258x_id_ok(0xff));

	formula_test();
	crop_test();

	/* 1x from four fifths full at 111x, 111x again below what 111x
	 * would read as 22200. */
	CHECK(tsl258x_next_gain(TSL258X_GAIN_111X, 29999) == TSL258X_GAIN_111X);
	CHECK(tsl258x_next_gain(TSL258X_GAIN_111X, 30000) == TSL258X_GAIN_1X);
	CHECK(tsl258x_next_gain(TSL258X_GAIN_111X, 0xffff) == TSL258X_GAIN_1X);
	CHECK(tsl258x_next_gain(TSL258X_GAIN_1X, 200) == TSL258X_GAIN_1X);
	CHECK(tsl258x_next_gain(TSL258X_GAIN_1X, 199) == TSL258X_GAIN_111X);

	driver_test();
	puts(failures ? "tsl258x_test: FAILED" : "tsl258x_test: ok");
	return failures != 0;
}
