/* The PAH8001 as the vendor's ADSP ran it, which is PixArt's reference
 * code: the register table on every start, the LED dimmed while no skin
 * is on the sensor and its current stepped to keep the exposure in
 * range, and the 32-bit sample read whenever the chip has a new frame.
 * It has no FIFO and no data-ready line, so it is polled every
 * POLL_MS while on, the watch kept awake meanwhile. Its frames come at
 * 20 Hz and are interpolated to the heart rate's 25 Hz, a lost frame
 * repeated. The sample's scale is not documented: it is taken from the
 * first one, and starts over when it runs out of range or the LED
 * changes. */
#include "asteroid.h"
#include "sensors/hub.h"
#include "sensors/hr.h"
#include "sensors/pah8001.h"

#define BUS		(board->hub_pins.i2c.bus)
#define ADDR		(board->hr_pins.addr)
#define GPIO_ON		(board->hr_pins.power)
#define GPIO_PD		(board->hr_pins.pd)

#define BANK		0x7f
#define ID		0x00
#define OP_MODE		0x05
#define CONFIG		0x06
#define TOUCH		0x59
#define EXPOSURE_LO	0x32
#define EXPOSURE_HI	0x33
/* In bank 1. */
#define SLEEP_CTRL	0x42
#define LED_STEP	0x38
#define READY		0x68
#define SAMPLE		0x64

#define PRODUCT_ID	0x30
#define RESET		0x82
#define POWER_DOWN	0x08
#define TOUCHED		0x80
#define LED_BASE	0xe0
#define STEP_IDLE	5
#define STEP_TOUCH	25
#define STEP_MAX	31
#define EXPOSURE_UP	496
#define EXPOSURE_DOWN	32
#define EXPOSURE_OK_LO	64
#define EXPOSURE_OK_HI	420
#define LED_EVERY	5

#define POLL_MS		40
#define FRAME_MS	50
#define OUT_MS		(1000 / HR_HZ)

/* PixArt's table, register and value with the bank switches in it. For
 * bank 0's 0x17 only bit 7 is set. */
#define SET_BIT7	0x100
static const uint16_t table[][2] = {
	{ BANK, 0x00 }, { 0x09, 0x5a }, { OP_MODE, 0x99 }, { 0x17, SET_BIT7 }, { 0x27, 0xff }, { 0x28, 0xfa },
	{ 0x29, 0x0a }, { 0x2a, 0xc8 }, { 0x2b, 0xa0 }, { 0x2c, 0x8c }, { 0x2d, 0x64 }, { 0x42, 0x20 },
	{ 0x48, 0x00 }, { 0x4d, 0x1a }, { 0x7a, 0xb5 },
	{ BANK, 0x01 }, { 0x07, 0x48 }, { 0x23, 0x28 }, { 0x26, 0x0f }, { 0x2e, 0x48 }, { 0x38, 0xea },
	{ 0x42, 0xa4 }, { 0x43, 0x41 }, { 0x44, 0x41 }, { 0x45, 0x24 }, { 0x46, 0xc0 }, { 0x52, 0x32 },
	{ 0x53, 0x28 }, { 0x56, 0x60 }, { 0x57, 0x28 }, { 0x6d, 0x02 }, { 0x0f, 0xc8 },
	{ BANK, 0x00 }, { 0x5d, 0x81 },
};

static bool present, on, touch, asleep, based;
static int dir;
static uint8_t step, bank = 0xff, polls;
static uint16_t exposure;
static uint32_t raw, base, poll_t, frames, lost, errors, readings;
static uint32_t rate = HUB_NONE;
static uint8_t quality;
/* The last frame's sample and how far the next output sample is past it. */
static int32_t prev;
static uint32_t out_at;

static int wr(uint8_t reg, uint8_t v)
{
	uint8_t b[2] = { reg, v };
	int r = i2c_xfer(BUS, ADDR, b, 2, NULL, 0);
	errors += r != 0;
	if (!r && reg == BANK)
		bank = v;
	else if (r && reg == BANK)
		bank = 0xff;
	return r;
}

static int rd(uint8_t reg, uint8_t *v, unsigned n)
{
	int r = i2c_xfer(BUS, ADDR, &reg, 1, v, n);
	errors += r != 0;
	return r;
}

static int to_bank(uint8_t b) { return bank == b ? 0 : wr(BANK, b); }

static int set_bits(uint8_t reg, uint8_t set, uint8_t clear)
{
	uint8_t v;
	int r = to_bank(0) || rd(reg, &v, 1);
	return r ? r : wr(reg, (uint8_t)((v & ~clear) | set));
}

static bool probe(void)
{
	uint8_t id = 0;
	return !to_bank(0) && !rd(ID, &id, 1) && id == PRODUCT_ID;
}

static void power_down(void)
{
	set_bits(CONFIG, POWER_DOWN, 0);
	gpio_set(GPIO_PD, 1);
}

void pah8001_init(void)
{
	if (!ADDR)
		return;
	gpio_set(GPIO_ON, 1);
	gpio_config(GPIO_ON, 0, GPIO_OUTPUT, GPIO_PULL_DOWN, 8);
	gpio_set(GPIO_PD, 1);
	gpio_config(GPIO_PD, 0, GPIO_OUTPUT, GPIO_PULL_DOWN, 8);
	delay_ms(40);
	wdt_feed();
	present = probe();
	if (present)
		power_down();
	log_printf("hr: %s\n", present ? "PAH8001, powered down" : "no PAH8001");
}

static void led(uint8_t s)
{
	step = s;
	if (!to_bank(1))
		wr(LED_STEP, (uint8_t)(LED_BASE | s));
	based = false;
	hr_reset();
}

/* The vendor's: dim and asleep without skin; with it, full and then two
 * steps at a time while the exposure is at an end of its range. */
static void led_ctrl(void)
{
	if (!touch) {
		if (!asleep && !to_bank(0) && !wr(OP_MODE, 0xb8) && !to_bank(1) && !wr(SLEEP_CTRL, 0xa0)) {
			led(STEP_IDLE);
			asleep = true;
		}
		return;
	}
	if (asleep) {
		if (to_bank(0) || wr(OP_MODE, 0x98) || to_bank(1) || wr(SLEEP_CTRL, 0xa4))
			return;
		asleep = false;
		dir = 0;
		polls = 0;
		led(STEP_TOUCH);
		return;
	}
	if (++polls < LED_EVERY)
		return;
	polls = 0;
	uint8_t e[2];
	if (to_bank(0) || rd(EXPOSURE_LO, e, 2))
		return;
	exposure = (uint16_t)((e[1] & 3) << 8 | e[0]);
	if (!dir)
		dir = exposure >= EXPOSURE_UP ? 1 : exposure <= EXPOSURE_DOWN ? -1 : 0;
	else if (exposure >= EXPOSURE_OK_LO && exposure <= EXPOSURE_OK_HI)
		dir = 0;
	if (!dir)
		return;
	int s = step + 2 * dir;
	led((uint8_t)(s < 1 ? 1 : s > STEP_MAX ? STEP_MAX : s));
}

/* Each frame stands for FRAME_MS; output samples every OUT_MS fall
 * between it and the one before. */
static void frame(int32_t x)
{
	bool fresh = false;
	while (out_at <= FRAME_MS) {
		fresh |= hr_add(prev + (x - prev) * (int32_t)out_at / FRAME_MS);
		out_at += OUT_MS;
	}
	out_at -= FRAME_MS;
	prev = x;
	if (fresh) {
		rate = hr_bpm(&quality);
		readings++;
	}
}

static void start(void)
{
	bool ok = probe() && !wr(CONFIG, RESET);
	delay_ms(10);
	bank = 0xff;
	for (unsigned i = 0; ok && i < ARRAY_SIZE(table); i++) {
		uint8_t reg = (uint8_t)table[i][0];
		if (table[i][1] & SET_BIT7)
			ok = !set_bits(reg, 0x80, 0);
		else
			ok = !wr(reg, (uint8_t)table[i][1]);
	}
	if (ok) {
		gpio_set(GPIO_PD, 0);
		delay_ms(3);
		ok = !set_bits(CONFIG, 0, POWER_DOWN);
	}
	step = 10;
	asleep = true;
	touch = based = false;
	rate = HUB_NONE;
	quality = 0;
	readings = 0;
	poll_t = time_ms();
	hr_reset();
	log_printf("hr: PAH8001 %s\n", ok ? "on" : "start failed");
}

void pah8001_on(bool want)
{
	if (!present || want == on)
		return;
	on = want;
	if (on) {
		start();
	} else {
		power_down();
		rate = HUB_NONE;
		log_printf("hr: off, %lu frames %lu lost %lu errors\n", frames, lost, errors);
	}
}

void pah8001_poll(void)
{
	uint32_t now = time_ms();
	if (!on || now - poll_t < POLL_MS)
		return;
	poll_t = now;
	uint8_t t, n, s[4];
	if (to_bank(0) || rd(TOUCH, &t, 1))
		return;
	bool was = touch;
	touch = t & TOUCHED;
	led_ctrl();
	if (!touch) {
		if (was) {
			hr_reset();
			based = false;
			rate = HUB_NONE;
		}
		return;
	}
	if (to_bank(1) || rd(READY, &n, 1) || !(n &= 0x0f) || rd(SAMPLE, s, 4))
		return;
	raw = (uint32_t)(s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24);
	int32_t x = (int32_t)(raw - base);
	if (!based || x >= HR_CLIP / 2 || x <= -HR_CLIP / 2) {
		base = raw;
		x = 0;
		prev = 0;
		out_at = 0;
		based = true;
		hr_reset();
	}
	frames += n;
	lost += n - 1u;
	while (n--)
		frame(x);
}

bool pah8001_can_sleep(void) { return !on; }

uint32_t pah8001_rate(uint8_t *status, unsigned *n)
{
	if (status)
		*status = quality;
	if (n)
		*n = on ? readings : 0;
	return rate;
}

void pah8001_status(void (*say)(const char *fmt, ...))
{
	if (!ADDR)
		return;
	char bpm[16] = "no heart rate";
	if (rate != HUB_NONE)
		snprintf(bpm, sizeof bpm, "%lu bpm", rate);
	say("hr: PAH8001 %s, %s, led step %u exposure %u, sample %08lx, %lu frames %lu lost %lu errors, %s\n",
	    !present ? "absent" : on ? "on" : "off", touch ? "skin" : "no skin", step, exposure, raw, frames, lost, errors, bpm);
}
