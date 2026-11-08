/* smelt's light sensor, on I2C next to the gauge and the backlight, read
 * once for each always-on frame. Its rail is on only for a reading and
 * the chip's control register is cleared before that, as the vendor
 * powers it: rail, 5 ms, power bit, 3 ms, off in the reverse order. The
 * registers go with the rail, so each reading writes them again. Nothing
 * else on this watch uses the rail. The interrupt line is not used.
 *
 * light_start begins an integration of about 49 ms, which runs while the
 * frame is drawn, and light_finish collects it. The gain carries over
 * from one reading to the next; a reading that needs the other gain is
 * taken again at once, so a reading can take two integrations. Counts
 * that make no sense at either gain, as in direct sun, give no reading.
 *
 * Read often, while the screen is on, the rail stays up in between
 * (light_keep), which leaves only the bus transfers.
 *
 * The vendor's driver reads the ID register but never checks it, so
 * there is no value to expect here: a chip that answers is the sensor,
 * and its ID goes to the log. */
#include "asteroid.h"
#include "display/display.h"
#include "sensors/tsl2584.h"

#define QUP		(board->light_pins.i2c.qup)
#define ADDR		(board->light_pins.i2c.addr)
#define LDO		(board->light_pins.ldo)
#define BUS_HZ		200000

#define CMD_SELECT	0x80
#define CMD_AUTO_INC	0x20

#define REG_CONTROL	0x00
#define REG_ITIME	0x01
#define REG_ANALOG	0x07
#define REG_ID		0x12
#define REG_DATA0LOW	0x14

#define CONTROL_POWER	0x01
#define CONTROL_ADC_EN	0x02
#define CONTROL_VALID	0x10
#define ITIME_49MS	0xee
#define GAIN_1X		0x00
#define GAIN_8X		0x01

#define RAIL_MS		5
#define POWER_MS	3
#define INTEGRATION_MS	49
#define TIMEOUT_MS	75

static bool ready, running, kept;
static enum tsl2584_mode mode = TSL2584_HIGH_LUX;
static uint32_t started;

static int wr(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { CMD_SELECT | reg, val };
	return i2c_qup_xfer(QUP, ADDR, b, 2, NULL, 0);
}

static int rd(uint8_t reg, uint8_t *v, unsigned n)
{
	uint8_t cmd = CMD_SELECT | reg;
	return i2c_qup_xfer(QUP, ADDR, &cmd, 1, v, n);
}

static int power_up(void)
{
	int r = rpm_regulator_enable(RPM_LDOA, LDO, true);
	if (r)
		return r;
	delay_ms(RAIL_MS);
	r = wr(REG_CONTROL, CONTROL_POWER);
	if (!r)
		delay_ms(POWER_MS);
	return r;
}

static void power_down(void)
{
	wr(REG_CONTROL, 0);
	rpm_regulator_enable(RPM_LDOA, LDO, false);
}

static int convert(void)
{
	int r = wr(REG_CONTROL, CONTROL_POWER);
	if (!r)
		r = wr(REG_ANALOG, mode == TSL2584_LOW_LUX ? GAIN_8X : GAIN_1X);
	if (!r)
		r = wr(REG_ITIME, ITIME_49MS);
	if (!r)
		r = wr(REG_CONTROL, CONTROL_ADC_EN | CONTROL_POWER);
	started = time_ms();
	return r;
}

static int measure(int32_t *lux)
{
	uint8_t status = 0, b[4];
	uint32_t gone = time_ms() - started;
	if (gone < INTEGRATION_MS)
		delay_ms(INTEGRATION_MS - gone);
	for (;;) {
		int r = rd(REG_CONTROL, &status, 1);
		if (r)
			return r;
		if (status & CONTROL_VALID)
			break;
		if (time_ms() - started > TIMEOUT_MS)
			return -1;
		delay_ms(1);
	}
	int r = rd(REG_DATA0LOW | CMD_AUTO_INC, b, sizeof b);
	if (r)
		return r;
	const uint16_t *coeff = board->light_pins.coeff[fb.panel_height == board->panel.height_alt];
	*lux = tsl2584_lux(mode, (uint16_t)(b[0] | b[1] << 8), (uint16_t)(b[2] | b[3] << 8), coeff);
	return 0;
}

static bool tsl2584_init(void)
{
	uint8_t id = 0;
	int r = i2c_qup_init(QUP, board->light_pins.i2c.sda, board->light_pins.i2c.scl, board->light_pins.i2c.func, BUS_HZ);
	if (!r)
		r = rpm_regulator_voltage(RPM_LDOA, LDO, board->light_pins.ldo_uv);
	if (!r)
		r = power_up();
	if (!r)
		r = rd(REG_ID, &id, 1);
	power_down();
	if (r) {
		log_printf("light: tsl2584 not found (%d)\n", r);
		return false;
	}
	ready = true;
	log_printf("light: tsl2584, id %02x\n", id);
	return true;
}

static void tsl2584_start(void)
{
	if (!ready)
		return;
	running = (kept || !power_up()) && !convert();
	if (!running && !kept)
		power_down();
}

static int32_t tsl2584_finish(void)
{
	if (!running)
		return -1;
	running = false;
	int32_t lux = 0;
	int r = measure(&lux);
	if (!r && tsl2584_next_mode(mode, lux) != mode) {
		mode = tsl2584_next_mode(mode, lux);
		r = convert();
		if (!r)
			r = measure(&lux);
		if (!r)
			mode = tsl2584_next_mode(mode, lux);
	}
	if (!kept)
		power_down();
	return r ? -1 : lux;
}

static void tsl2584_keep(bool on)
{
	if (!ready || on == kept)
		return;
	running = false;
	if (on) {
		kept = !power_up();
		if (!kept)
			power_down();
	} else {
		kept = false;
		power_down();
	}
}

const struct light_driver light_tsl2584 = {
	.init = tsl2584_init,
	.start = tsl2584_start,
	.finish = tsl2584_finish,
	.keep = tsl2584_keep,
};
