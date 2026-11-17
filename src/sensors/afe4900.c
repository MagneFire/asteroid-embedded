/* The AFE4900 optical heart rate front end, as the vendor's firmware
 * runs it: LED3 and an ambient phase, 20 sample sets a FIFO batch with
 * ADC_RDY rising for each. A start measures the ambient light at
 * 200 Hz, sets the offset DAC, LED current and TIA gain from it in a
 * few batches, then samples at 25 Hz; LED3 minus ambient goes to
 * hr.c. A saturated LED phase starts the calibration over. The
 * interrupt only marks a batch; the main loop reads it. */
#include "asteroid.h"
#include "sensors/afe4900.h"
#include "sensors/hr.h"
#include "sensors/hub.h"

#define P		(board->hr_pins)
#define SETS		20
#define FULL		(1 << 21)

#define CONTROL0	0x00
#define PRPCT		0x1d
#define TIMER		0x1e
#define TIA_GAIN	0x21
#define LED_CURRENT	0x22
#define OFFDAC		0x3a
#define OFFDAC_LSB	0x3e
#define SLEEP_START	0x6a
#define SLEEP_END	0x6b
#define FIFO		0xff
#define CTRL_RUN	0x60
#define CTRL_READ	0x61
#define TIMEREN		BIT(8)
#define PRPCT_25HZ	0x13ff
#define PRPCT_200HZ	0x27f

/* The vendor's table, register << 24 | value, in its order. */
static const uint32_t init_regs[] = {
	0x00000060, 0x09000027, 0x0a00002f, 0x0100002b, 0x0200002f, 0x0d000031,
	0x0e000041, 0x36000031, 0x37000039, 0x05000035, 0x06000039, 0x0f000043,
	0x10000053, 0x0300003b, 0x04000043, 0x0700003f, 0x08000043, 0x11000055,
	0x12000065, 0x45000027, 0x46000044, 0x4700ffff, 0x4800ffff, 0x4900ffff,
	0x4a00ffff, 0x5700ffff, 0x5800ffff, 0x43000045, 0x4400004d, 0x0b000049,
	0x0c00004d, 0x13000067, 0x14000077, 0x5200007d, 0x5300007d, 0x64000000,
	0x65000079, 0x66000000, 0x67000079, 0x68000000, 0x69000079, 0x6a000084,
	0x6b0013e1, 0x1d0013ff, 0x1e000001, 0x21003015, 0x23124218, 0x31000020,
	0x420004e0, 0x4b000009, 0x50000018, 0x6c000800, 0x720000f0, 0x3a184210,
};

/* Gain steps 1-9: the TIA's Rf and Cf codes, and Rf in kOhm. */
static const uint8_t gain_bits[9] = { 0x35, 0x34, 0x33, 0x22, 0x11, 0x00, 0x0e, 0x48, 0x0f };
static const uint16_t rf_k[9] = { 10, 25, 50, 100, 250, 500, 1000, 1500, 2000 };

enum { OFF, AMBIENT, LED_START, LED_FIT, GAIN, SETTLE, RUN };
static uint8_t state, gain, led, steps, quality;
static int8_t off_led, off_rest;
static bool offset_used, coarse_done;
static volatile bool irq_seen;
static uint32_t rate = HUB_NONE, batches, errors, restarts;
static unsigned readings, logged;
static uint8_t fifo[SETS * 12];

static int wr(uint8_t reg, uint32_t v)
{
	uint8_t b[4] = { reg, (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
	int r = iom_spi_write(P.spi_bus, P.spi_cs, b, 4, false);
	errors += r != 0;
	return r;
}

static uint32_t rd(uint8_t reg)
{
	uint8_t b[3] = { 0 };
	wr(CONTROL0, CTRL_READ);
	if (iom_spi_write(P.spi_bus, P.spi_cs, &reg, 1, true) || iom_spi_read(P.spi_bus, P.spi_cs, b, 3, false))
		errors++;
	wr(CONTROL0, CTRL_RUN);
	return (uint32_t)b[0] << 16 | b[1] << 8 | b[2];
}

static void set_rate(uint32_t prpct)
{
	wr(PRPCT, prpct);
	wr(SLEEP_START, 0x84);
	wr(SLEEP_END, prpct - 30);
}

static void set_led_gain(void)
{
	wr(LED_CURRENT, (uint32_t)(led >> 2) << 12 | (uint32_t)(led & 3) << 22);
	wr(TIA_GAIN, gain_bits[gain - 1]);
}

/* The offset DAC: off_led for LED3's phase, off_rest for the other
 * three, each a sign and 7 bits spread over both registers. */
static void set_offsets(void)
{
	unsigned m3 = (unsigned)(off_led < 0 ? -off_led : off_led), mo = (unsigned)(off_rest < 0 ? -off_rest : off_rest);
	uint32_t f3 = (m3 >> 2 & 15) | (off_led >= 0) << 4, fo = (mo >> 2 & 15) | (off_rest >= 0) << 4;
	uint32_t l3 = (m3 >> 5 & 2) | (m3 >> 1 & 1), lo = (mo >> 5 & 2) | (mo >> 1 & 1);
	wr(OFFDAC, BIT(20) | f3 | fo << 5 | fo << 10 | fo << 15);
	wr(OFFDAC_LSB, l3 | lo << 2 | lo << 4 | lo << 6 | (m3 & 1) << 8 | (mo & 1) * (BIT(9) | BIT(10) | BIT(11)));
}

static int8_t clamp127(int32_t v) { return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v); }

static void start(void)
{
	wr(OFFDAC, 0);
	wr(OFFDAC_LSB, 0);
	off_led = off_rest = 0;
	led = 3;
	gain = 3;
	steps = 0;
	set_led_gain();
	set_rate(PRPCT_200HZ);
	state = AMBIENT;
}

/* The LED current that brings the signal to the vendor's level, and
 * LED3's offset just below it. */
static void fit(int32_t diff)
{
	float rf = rf_k[gain - 1];
	float x = (float)diff + 874.0f * rf * (float)(off_led - off_rest);
	bool pos = x >= 0;
	int32_t unit = (int32_t)((pos ? x : -x) * 10 / (28.0f * led * rf));
	if (!unit)
		unit = 1;
	int32_t code = (1250 * 16 / unit + 8) >> 4;
	code = code < 13 ? 13 : code > 255 ? 255 : code;
	float level = (float)unit * (float)code;
	int32_t step = (int32_t)(level * 4 / 1250 + 0.5f);
	gain = level <= 1500 ? 5 : 4;
	led = (uint8_t)code;
	off_led = clamp127(off_rest + (pos ? step : -step));
	set_offsets();
	set_led_gain();
}

static void control(int32_t amb, int32_t lv)
{
	int32_t diff = lv - amb;
	int32_t ad = diff < 0 ? -diff : diff, al = lv < 0 ? -lv : lv;
	if (state < SETTLE && ++steps > 40) {
		log_printf("hr: cal not settled\n");
		set_rate(PRPCT_25HZ);
		state = SETTLE;
		return;
	}
	if (state < SETTLE)
		log_printf("hr: cal %u amb %ld led %ld, code %u gain %u off %d/%d\n", state, amb, lv, led, gain,
			   off_led, off_rest);
	switch (state) {
	case AMBIENT: {
		int32_t n = ((amb < 0 ? -amb : amb) + 21845) / 43690;
		off_led = off_rest = clamp127(amb < 0 ? -n : n);
		offset_used = n != 0;
		set_offsets();
		state = LED_START;
		break;
	}
	case LED_START:
		led = 3;
		gain = offset_used ? 5 : 7;
		set_led_gain();
		coarse_done = false;
		state = LED_FIT;
		break;
	case LED_FIT:
		if (ad > FULL / 2 && gain > 1) {
			gain--;
			set_led_gain();
		} else {
			fit(diff);
			state = GAIN;
		}
		break;
	case GAIN:
		if (diff < 0 && !coarse_done) {
			int32_t n = 1 + (int32_t)((float)-diff / (87381.33f * (gain == 5 ? 2.5f : 1)));
			off_led = clamp127(off_led - (n > 7 ? 7 : n));
			set_offsets();
		} else if (ad <= 20971 && al < FULL / 5 && gain < 9) {
			gain++;
			set_led_gain();
		} else if (al > FULL / 10 * 9 && gain > 1) {
			gain--;
			set_led_gain();
		} else {
			set_rate(PRPCT_25HZ);
			state = SETTLE;
			break;
		}
		coarse_done = true;
		break;
	case SETTLE:
		hr_reset();
		state = RUN;
		log_printf("hr: 25 Hz, code %u gain %u off %d/%d\n", led, gain, off_led, off_rest);
		break;
	case RUN:
		if (al > FULL / 100 * 93) {
			restarts++;
			log_printf("hr: led %ld saturated, again\n", lv);
			start();
		}
		break;
	}
}

static int32_t s24(const uint8_t *b)
{
	return (int32_t)((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8) >> 8;
}

static void line_up(unsigned pin) { irq_seen = true; }

static void power_up(void)
{
	for (const struct board_mcu_pad *p = P.pads; p->func != 0xff; p++)
		pad_set(p);
	iom_spi_init(P.spi_bus, P.spi_hz, 0);
	gpio_set(P.power, 1);
	gpio_config(P.power, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_set(P.reset, 1);
	gpio_config(P.reset, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	delay_ms(15);
	gpio_set(P.reset, 0);
	delay_us(37);
	gpio_set(P.reset, 1);
	delay_ms(10);
	gpio_config(P.irq, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	gpio_irq_config(P.irq, GPIO_IRQ_EDGE_RISING, line_up);
	gpio_irq_enable(P.irq, true);
}

/* No register is written: the chip loses reset, then its supply. The
 * pads are left as at boot. */
static void power_down(void)
{
	gpio_irq_enable(P.irq, false);
	gpio_set(P.reset, 0);
	delay_ms(2);
	gpio_set(P.power, 0);
	iom_stop(P.spi_bus);
	delay_ms(10);
	gpio_config(P.reset, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	pad_config(P.irq, PAD_FUNC_GPIO, 0);
	for (const struct board_mcu_pad *p = P.pads; p->func != 0xff; p++)
		pad_config(p->pad, PAD_FUNC_GPIO, 0);
}

void afe4900_on(bool on)
{
	if (!P.pads || on == (state != OFF))
		return;
	rate = HUB_NONE;
	readings = logged = 0;
	quality = 0;
	if (!on) {
		power_down();
		state = OFF;
		log_printf("hr: off, %lu batches %lu errors\n", batches, errors);
		return;
	}
	power_up();
	for (unsigned i = 0; i < ARRAY_SIZE(init_regs); i++)
		wr((uint8_t)(init_regs[i] >> 24), init_regs[i] & 0xffffff);
	start();
	uint32_t t = rd(TIMER);
	wr(TIMER, t | TIMEREN);
	irq_seen = false;
	log_printf("hr: AFE4900 on, timer %06lx\n", t);
}

void afe4900_poll(void)
{
	if (state == OFF)
		return;
	if (!irq_seen && !gpio_get(P.irq))
		return;
	irq_seen = false;
	uint8_t reg = FIFO;
	if (iom_spi_write(P.spi_bus, P.spi_cs, &reg, 1, true) ||
	    iom_spi_read(P.spi_bus, P.spi_cs, fifo, sizeof(fifo), false)) {
		errors++;
		return;
	}
	batches++;
	int32_t amb = 0, lv = 0;
	bool fresh = false;
	for (unsigned i = 0; i < SETS; i++) {
		int32_t a = s24(fifo + 12 * i), l = s24(fifo + 12 * i + 3);
		if (i >= 5) {
			amb += a;
			lv += l;
		}
		if (state == RUN)
			fresh |= hr_add(l >= HR_CLIP || l <= -HR_CLIP || a >= HR_CLIP || a <= -HR_CLIP ? HR_CLIP : l - a);
	}
	control(amb / (SETS - 5), lv / (SETS - 5));
	if (fresh) {
		rate = hr_bpm(&quality);
		readings++;
		if (rate != logged)
			log_printf("hr: %lu bpm q%u\n", rate, quality);
		logged = rate;
	}
}

bool afe4900_can_sleep(void) { return state == OFF || (!irq_seen && !gpio_get(P.irq)); }

uint32_t afe4900_rate(uint8_t *status, unsigned *n)
{
	if (status)
		*status = quality;
	if (n)
		*n = state != OFF ? readings : 0;
	return rate;
}

void afe4900_status(void (*say)(const char *fmt, ...))
{
	if (P.pads)
		say("hr: AFE4900 %s, %lu batches %lu errors %lu restarts, code %u gain %u off %d/%d, %lu bpm\n",
		    state == OFF ? "off" : state == RUN ? "running" : "calibrating", batches, errors, restarts, led, gain,
		    off_led, off_rest, rate);
}
