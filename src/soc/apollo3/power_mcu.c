/* The keys and the battery as a microcontroller sees them. The keys are
 * GPIOs, low while pressed. The battery is only its voltage on the ADC:
 * the charger and the gauge belong to the application processor's PMIC,
 * so the charge is read off a Li-ion resting voltage curve, averaged over
 * the last readings, and there is no charging state. A charge from the
 * application processor's gauge is used instead while it is recent. */
#include "asteroid.h"

#define KEY		(board->key_gpio[0])
#define READ_MS		60000u
#define AVERAGE		4
#define AP_FRESH_MS	(30u * 60 * 1000)

static volatile bool key_edge;

static void key_irq(unsigned pin) { key_edge = true; }

bool power_key(void) { return KEY && !gpio_get(KEY); }

const char *power_key_name(void) { return NULL; }

void power_key_irq_init(void)
{
	for (unsigned i = 0; i < ARRAY_SIZE(board->key_gpio); i++) {
		unsigned pin = board->key_gpio[i];
		if (!pin)
			continue;
		gpio_config(pin, 0, GPIO_INPUT, GPIO_PULL_UP, 2);
		if (i == 0) {
			gpio_irq_config(pin, GPIO_IRQ_EDGE_FALLING, key_irq);
			gpio_irq_enable(pin, true);
		}
	}
}

bool power_key_irq(void)
{
	bool e = key_edge;
	key_edge = false;
	return e;
}

static const struct { uint16_t mv; uint8_t pct; } curve[] = {
	{ 3400, 0 }, { 3600, 5 }, { 3700, 15 }, { 3750, 25 }, { 3800, 40 }, { 3850, 50 },
	{ 3900, 60 }, { 3980, 70 }, { 4060, 80 }, { 4150, 90 }, { 4300, 100 },
};

static uint32_t mv_avg, readings[AVERAGE], last_read, ap_percent, ap_t;
static unsigned count;

static uint32_t percent_of(uint32_t mv)
{
	if (mv <= curve[0].mv)
		return 0;
	for (unsigned i = 1; i < ARRAY_SIZE(curve); i++)
		if (mv < curve[i].mv)
			return curve[i - 1].pct + (mv - curve[i - 1].mv) * (curve[i].pct - curve[i - 1].pct) /
							  (curve[i].mv - curve[i - 1].mv);
	return 100;
}

static void read_now(void)
{
	uint32_t mv = adc_battery_mv();
	last_read = time_ms() | 1;
	if (!mv)
		return;
	readings[count++ % AVERAGE] = mv;
	unsigned n = count < AVERAGE ? count : AVERAGE;
	uint32_t sum = 0;
	for (unsigned i = 0; i < n; i++)
		sum += readings[i];
	mv_avg = sum / n;
}

static void adc_init(void) { read_now(); }

static void adc_poll(void)
{
	if (!last_read || time_ms() - last_read >= READ_MS)
		read_now();
}

static uint32_t adc_next_ms(void) { return last_read ? ms_left(last_read, READ_MS, time_ms()) : 1; }
static bool ap_fresh(void) { return ap_t && time_ms() - ap_t < AP_FRESH_MS; }

void battery_from_ap(unsigned percent)
{
	ap_percent = percent;
	ap_t = time_ms() | 1;
}

static uint32_t adc_percent(void) { return ap_fresh() ? ap_percent : mv_avg ? percent_of(mv_avg) : 50; }
static bool no(void) { return false; }
static void nothing(void) {}
static uint32_t adc_mv(void) { return mv_avg; }
static int32_t no_current(void) { return 0; }
static const char *adc_status(void) { return ap_fresh() ? "from the AP" : mv_avg ? "voltage only" : "no reading"; }

const struct battery_driver battery_adc = {
	.init = adc_init, .poll = adc_poll, .next_ms = adc_next_ms, .percent = adc_percent, .charging = no, .cable = no,
	.usb = no, .cable_irq_init = nothing, .cable_irq = no, .mv = adc_mv, .ua = no_current, .avg_ua = no_current,
	.status = adc_status,
};
