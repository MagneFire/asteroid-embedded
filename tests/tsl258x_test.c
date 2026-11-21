/* beluga's TSL258x: its lux and gain sums and its driver. */
#include <stdio.h>
#include <string.h>
#include "sensors/light_tsl258x.c"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const struct board test_board = {
	.light_pins = { .i2c = { .bus = 0, .sda = 6, .scl = 5, .addr = 0x29 } },
};
const struct board *board = &test_board;

/* The chip: its registers, channel 0 at each gain, and whether a whole
 * integration has run since the ADC went on. */
static uint8_t regs[0x20], id = 0x94;
static uint16_t counts[2];
static bool absent, cleared;
static unsigned transfers;
static uint32_t now;
static int fail_after = -1;

uint32_t time_ms(void) { return now; }
void log_printf(const char *fmt, ...) {}
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

	/* Found: powered off, its interrupt off and cleared. */
	id = 0x94;
	regs[REG_CONTROL] = CONTROL_POWER;
	regs[REG_INTERRUPT] = 0x12;
	CHECK(d->init());
	CHECK(!regs[REG_CONTROL] && !regs[REG_INTERRUPT] && cleared);

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
	CHECK(reading() == 4500 * 300 / (111 * 37));

	/* Each later reading is two transfers and no waiting. */
	transfers = 0;
	uint32_t t = now;
	d->start();
	CHECK(d->finish() == 328);
	CHECK(transfers == 2 && now == t);

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
	CHECK(reading() == 400 * 300 / 37);

	/* Dim at 1x goes back to 111x. */
	counts[0] = 199;
	counts[1] = 22000;
	CHECK(reading() == -1 && gain == TSL258X_GAIN_111X);
	now += 250;
	CHECK(reading() == 22000 * 300 / (111 * 37));

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
	CHECK(reading() == 1607);

	/* Letting go powers it down; keeping it again starts over. */
	d->keep(false);
	CHECK(!regs[REG_CONTROL]);
	transfers = 0;
	CHECK(reading() == -1 && transfers == 0);
	d->keep(true);
	CHECK(reading() == -1);
	CHECK(reading() == 1607);
	d->keep(true);
	CHECK(reading() == 1607);

	/* A chip that stops answering is not kept. */
	d->keep(false);
	fail_after = 0;
	d->keep(true);
	CHECK(!kept && !regs[REG_CONTROL]);
	fail_after = -1;
	CHECK(reading() == -1);
}

int main(void)
{
	/* A TSL258x is 0x9n. */
	CHECK(tsl258x_id_ok(0x90) && tsl258x_id_ok(0x94) && tsl258x_id_ok(0x9f));
	CHECK(!tsl258x_id_ok(0x80) && !tsl258x_id_ok(0xa0) && !tsl258x_id_ok(0x00) && !tsl258x_id_ok(0xff));

	/* The vendor's steps at 111x: 90, 1000 and 4500 counts. */
	CHECK(tsl258x_lux(TSL258X_GAIN_111X, 0) == 0);
	CHECK(tsl258x_lux(TSL258X_GAIN_111X, 90) == 6);
	CHECK(tsl258x_lux(TSL258X_GAIN_111X, 1000) == 73);
	CHECK(tsl258x_lux(TSL258X_GAIN_111X, 4500) == 328);
	/* Full at 111x, and 1x 111 times that. */
	CHECK(tsl258x_lux(TSL258X_GAIN_111X, 37887) == 2767);
	CHECK(tsl258x_lux(TSL258X_GAIN_1X, 37887) == 307191);
	CHECK(tsl258x_lux(TSL258X_GAIN_1X, 0xffff) == 531364);
	CHECK(tsl258x_lux(TSL258X_GAIN_1X, 1) == 8);

	/* 1x from four fifths full at 111x, 111x again below what 111x
	 * would read as 22200. */
	CHECK(tsl258x_next_gain(TSL258X_GAIN_111X, 29999) == TSL258X_GAIN_111X);
	CHECK(tsl258x_next_gain(TSL258X_GAIN_111X, 30000) == TSL258X_GAIN_1X);
	CHECK(tsl258x_next_gain(TSL258X_GAIN_111X, 0xffff) == TSL258X_GAIN_1X);
	CHECK(tsl258x_next_gain(TSL258X_GAIN_1X, 200) == TSL258X_GAIN_1X);
	CHECK(tsl258x_next_gain(TSL258X_GAIN_1X, 199) == TSL258X_GAIN_111X);
	/* The switch points agree within the hysteresis. */
	CHECK(tsl258x_lux(TSL258X_GAIN_1X, 200) < tsl258x_lux(TSL258X_GAIN_111X, 30000));

	driver_test();
	puts(failures ? "tsl258x_test: FAILED" : "tsl258x_test: ok");
	return failures != 0;
}
