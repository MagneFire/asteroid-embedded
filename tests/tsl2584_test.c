/* The TSL2584's lux and backlight sums and its driver. */
#include <stdio.h>
#include <string.h>
#include "sensors/light_tsl2584.c"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const uint16_t large[4] = { 1627, 2485, 1536, 2259 };
static const uint16_t small[4] = { 1932, 3054, 1726, 2539 };

static const struct board test_board = {
	.panel = { .height = 330, .height_alt = 325 },
	.light_pins = { .i2c = { .bus = 3, .addr = 0x29 }, .ldo = 18, .ldo_uv = 2950000,
			.coeff = { { 1627, 2485, 1536, 2259 }, { 1932, 3054, 1726, 2539 } } },
};
const struct board *board = &test_board;
struct fb fb = { .panel_height = 330 };

/* The chip's registers, the rail and the clock. Counts are per gain. */
static uint8_t regs[0x20];
static uint16_t counts[2][2];
static bool absent, rail, never_valid;
static unsigned rail_on_votes, conversions, rail_uv;
static uint32_t now;
static int fail_after = -1;

uint32_t time_ms(void) { return now; }
void delay_ms(uint32_t ms) { now += ms; }
void log_printf(const char *fmt, ...) {}
int i2c_init(unsigned bus, unsigned sda, unsigned scl, unsigned func, uint32_t bus_hz) { return 0; }
int rpm_regulator_voltage(uint32_t type, uint32_t id, uint32_t uv)
{
	rail_uv = uv;
	return 0;
}
int rpm_regulator_enable(uint32_t type, uint32_t id, bool on)
{
	CHECK(type == RPM_LDOA && id == 18);
	rail = on;
	rail_on_votes += on;
	if (!on)
		memset(regs, 0, sizeof regs);
	return 0;
}
int i2c_xfer(unsigned bus, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	CHECK(bus == 3 && addr == 0x29 && wn && (w[0] & CMD_SELECT));
	if (absent || !rail || !fail_after)
		return -2;
	if (fail_after > 0)
		fail_after--;
	uint8_t reg = w[0] & 0x1f;
	if (wn == 2) {
		regs[reg] = w[1];
		if (reg == REG_CONTROL && (w[1] & CONTROL_ADC_EN)) {
			unsigned gain = regs[REG_ANALOG] == GAIN_8X;
			CHECK(regs[REG_ITIME] == ITIME_49MS);
			regs[REG_DATA0LOW] = (uint8_t)counts[gain][0];
			regs[REG_DATA0LOW + 1] = (uint8_t)(counts[gain][0] >> 8);
			regs[REG_DATA0LOW + 2] = (uint8_t)counts[gain][1];
			regs[REG_DATA0LOW + 3] = (uint8_t)(counts[gain][1] >> 8);
			conversions++;
			if (!never_valid)
				regs[REG_CONTROL] |= CONTROL_VALID;
		}
		return 0;
	}
	if (reg == REG_DATA0LOW)
		CHECK(w[0] & CMD_AUTO_INC);
	for (unsigned i = 0; i < rn; i++)
		r[i] = regs[reg + i];
	if (reg == REG_ID)
		*r = 0x90;
	return 0;
}

static void set_counts(unsigned gain8, uint16_t c0, uint16_t c1)
{
	counts[gain8][0] = c0;
	counts[gain8][1] = c1;
}

static int32_t reading(void)
{
	const struct light_driver *d = &light_tsl2584;
	d->start();
	int32_t lux = d->finish();
	/* Off between readings, both the chip and its rail. */
	CHECK(!rail && !regs[REG_CONTROL]);
	return lux;
}

static void driver_test(void)
{
	const struct light_driver *d = &light_tsl2584;

	/* Nothing on the bus: no reading, and no more rail votes after. */
	absent = true;
	CHECK(!d->init());
	CHECK(!rail);
	unsigned votes = rail_on_votes;
	CHECK(reading() == -1);
	CHECK(rail_on_votes == votes);
	absent = false;

	CHECK(d->init());
	CHECK(rail_uv == 2950000 && !rail);

	/* From 1x, dim light is read again at 8x, and 8x stays for next time. */
	set_counts(0, 10, 3);
	set_counts(1, 80, 24);
	conversions = 0;
	now = 1000;
	CHECK(reading() == (1627 * 80 - 2485 * 24) / 1024);
	CHECK(conversions == 2);
	/* The rail, the chip and two integrations, well under 150 ms. */
	CHECK(now - 1000 == RAIL_MS + POWER_MS + 2 * INTEGRATION_MS);
	CHECK(mode == TSL2584_LOW_LUX);
	conversions = 0;
	CHECK(reading() == (1627 * 80 - 2485 * 24) / 1024);
	CHECK(conversions == 1);

	/* Time already spent drawing comes off the wait. */
	now = 2000;
	d->start();
	now += 100;
	CHECK(d->finish() > 0);
	CHECK(now == 2000 + RAIL_MS + POWER_MS + 100);

	/* A jump to bright overflows at 8x and is read again at 1x. */
	set_counts(1, 18432, 18432);
	set_counts(0, 1000, 300);
	conversions = 0;
	CHECK(reading() == (1627 * 1000 - 2485 * 300) / 128);
	CHECK(conversions == 2 && mode == TSL2584_HIGH_LUX);

	/* Overflowing at both gains is no reading. */
	set_counts(0, 18432, 18432);
	CHECK(reading() == -1);
	CHECK(mode == TSL2584_HIGH_LUX);

	/* A bus failure during a reading still powers it off. */
	set_counts(0, 1000, 300);
	fail_after = 5;
	d->start();
	CHECK(rail);
	fail_after = 0;
	CHECK(d->finish() == -1);
	CHECK(!rail);
	fail_after = -1;
	CHECK(reading() > 0);

	/* Kept on, the rail stays up between readings and a start waits
	 * for nothing; letting go powers it off. */
	votes = rail_on_votes;
	d->keep(true);
	CHECK(rail && rail_on_votes == votes + 1);
	now = 4000;
	d->start();
	CHECK(now == 4000);
	now += 60;
	CHECK(d->finish() > 0);
	CHECK(now == 4060 && rail && regs[REG_CONTROL] & CONTROL_POWER);
	d->start();
	CHECK(rail_on_votes == votes + 1);
	d->keep(true);
	CHECK(rail_on_votes == votes + 1);
	d->keep(false);
	CHECK(!rail && !regs[REG_CONTROL]);
	CHECK(d->finish() == -1);
	d->keep(false);
	CHECK(reading() > 0);

	/* A conversion that never ends gives up. */
	never_valid = true;
	now = 3000;
	CHECK(reading() == -1);
	CHECK(now - 3000 <= TIMEOUT_MS + RAIL_MS + POWER_MS + 1);
	never_valid = false;

	/* The small watch has its own coefficients. */
	fb.panel_height = 325;
	CHECK(reading() == (1932 * 1000 - 3054 * 300) / 128);
	fb.panel_height = 330;
}

int main(void)
{
	/* The larger mix, over 128 at 1x and 1024 at 8x. */
	CHECK(tsl2584_lux(TSL2584_HIGH_LUX, 1000, 300, large) == 6886);
	CHECK(tsl2584_lux(TSL2584_LOW_LUX, 1000, 300, large) == 860);
	CHECK(tsl2584_lux(TSL2584_HIGH_LUX, 1000, 300, small) == 7935);
	/* With more of channel 1 the second mix wins. */
	CHECK(tsl2584_lux(TSL2584_HIGH_LUX, 1000, 600, large) == 1410);
	/* Never under 2. */
	CHECK(tsl2584_lux(TSL2584_LOW_LUX, 0, 0, large) == 2);
	CHECK(tsl2584_lux(TSL2584_HIGH_LUX, 0, 0, large) == 2);
	/* Below zero: only 8x with a real count is out of range. */
	CHECK(tsl2584_lux(TSL2584_LOW_LUX, 201, 201, large) == TSL2584_OVERRANGE);
	CHECK(tsl2584_lux(TSL2584_LOW_LUX, 200, 200, large) == 2);
	CHECK(tsl2584_lux(TSL2584_HIGH_LUX, 18432, 18432, large) == 2);
	/* The largest counts fit. */
	CHECK(tsl2584_lux(TSL2584_HIGH_LUX, 0xffff, 0, small) == 989168);

	/* The modes change at 200 lux going up and 100 going down. */
	CHECK(tsl2584_next_mode(TSL2584_LOW_LUX, 199) == TSL2584_LOW_LUX);
	CHECK(tsl2584_next_mode(TSL2584_LOW_LUX, 200) == TSL2584_HIGH_LUX);
	CHECK(tsl2584_next_mode(TSL2584_LOW_LUX, TSL2584_OVERRANGE) == TSL2584_HIGH_LUX);
	CHECK(tsl2584_next_mode(TSL2584_HIGH_LUX, 100) == TSL2584_HIGH_LUX);
	CHECK(tsl2584_next_mode(TSL2584_HIGH_LUX, 99) == TSL2584_LOW_LUX);

	/* The curve's ends and where its parts meet. */
	CHECK(tsl2584_backlight_code(0) == 2);
	CHECK(tsl2584_backlight_code(2) == 3);
	CHECK(tsl2584_backlight_code(85) == 28);
	CHECK(tsl2584_backlight_code(86) == 28);
	CHECK(tsl2584_backlight_code(87) == 30);
	CHECK(tsl2584_backlight_code(88) == 30);
	CHECK(tsl2584_backlight_code(265) == 35);
	CHECK(tsl2584_backlight_code(266) == 36);
	CHECK(tsl2584_backlight_code(999) == 36);
	CHECK(tsl2584_backlight_code(1000) == 36);
	CHECK(tsl2584_backlight_code(2330) == 36);
	CHECK(tsl2584_backlight_code(989168) == 36);
	/* It only goes up and never passes the ambient maximum. */
	for (uint32_t l = 1; l < 3000; l++) {
		CHECK(tsl2584_backlight_code(l) >= tsl2584_backlight_code(l - 1));
		CHECK(tsl2584_backlight_code(l) <= 36);
	}

	driver_test();
	puts(failures ? "tsl2584_test: FAILED" : "tsl2584_test: ok");
	return failures != 0;
}
