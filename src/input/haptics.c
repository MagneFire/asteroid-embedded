/* DRV2605 haptic driver on BLSP1 QUP2, enabled by gpio 60, with the ERM
 * motor settings of the vendor kernel: closed loop, library D, rated
 * 0x7e, overdrive clamp 0xa0 during the auto-calibration at init and
 * 0x96 afterwards, signed real-time playback. A pattern is a list of
 * on/off durations played from the main loop in real-time playback mode. */
#include "asteroid.h"

#define QUP			2
#define ADDR			0x5a
#define GPIO_TRIGGER		59
#define GPIO_ENABLE		60

#define REG_STATUS		0x00
#define REG_MODE		0x01
#define REG_RTP			0x02
#define REG_LIBRARY		0x03
#define REG_GO			0x0c
#define REG_RATED		0x16
#define REG_CLAMP		0x17
#define REG_CAL_COMP		0x18
#define REG_CAL_BEMF		0x19
#define REG_FEEDBACK		0x1a
#define REG_CONTROL2		0x1c
#define REG_CONTROL3		0x1d

#define MODE_INTERNAL		0x00
#define MODE_RTP		0x05
#define MODE_CALIBRATE		0x07
#define MODE_STANDBY		0x40
#define FEEDBACK_LRA		0x80
#define CONTROL2_BIDIR		0x80
#define CONTROL3_ERM_OPEN	0x20
#define CONTROL3_LRA_OPEN	0x01
#define CONTROL3_UNSIGNED	0x08
#define STATUS_DIAG		0x08

static bool ready;
static const uint16_t *pattern;
static unsigned steps, step;
static uint32_t step_t;

static int write_reg(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { reg, val };
	return i2c_qup_xfer(QUP, ADDR, b, 2, NULL, 0);
}

static int read_reg(uint8_t reg, uint8_t *val)
{
	return i2c_qup_xfer(QUP, ADDR, &reg, 1, val, 1);
}

static int update_reg(uint8_t reg, uint8_t mask, uint8_t val)
{
	uint8_t v;
	int r = read_reg(reg, &v);
	return r ? r : write_reg(reg, (uint8_t)((v & ~mask) | val));
}

static int calibrate(uint8_t *status)
{
	int r;
	if ((r = write_reg(REG_MODE, MODE_CALIBRATE)) || (r = write_reg(REG_GO, 1)))
		return r;
	uint32_t t0 = time_ms();
	uint8_t go = 1;
	while (go & 1) {
		if (time_ms() - t0 > 2000)
			return -10;
		delay_ms(10);
		wdt_feed();
		if ((r = read_reg(REG_GO, &go)))
			return r;
	}
	return read_reg(REG_STATUS, status);
}

bool haptics_init(void)
{
	tlmm_config(GPIO_TRIGGER, 0, TLMM_OUTPUT, TLMM_PULL_DOWN, 2);
	tlmm_set(GPIO_TRIGGER, 0);
	tlmm_config(GPIO_ENABLE, 0, TLMM_OUTPUT, TLMM_PULL_DOWN, 2);
	tlmm_set(GPIO_ENABLE, 1);
	delay_ms(10);
	if (i2c_qup_init(QUP, 6, 7, 3, 384000))
		return false;
	uint8_t status, feedback, bemf;
	int r;
	if ((r = read_reg(REG_STATUS, &status))) {
		log_printf("haptics: no answer (%d)\n", r);
		return false;
	}
	if ((r = write_reg(REG_MODE, MODE_INTERNAL)))
		return false;
	if (!read_reg(REG_FEEDBACK, &feedback) && !read_reg(REG_CAL_BEMF, &bemf) &&
	    ((feedback & FEEDBACK_LRA) || bemf == 0xff)) {
		write_reg(REG_FEEDBACK, 0x36);
		write_reg(REG_CAL_BEMF, 0x6d);
	}
	if ((r = write_reg(REG_LIBRARY, 0x04)) || (r = write_reg(REG_RATED, 0x7e)) || (r = write_reg(REG_CLAMP, 0xa0)) ||
	    (r = update_reg(REG_FEEDBACK, FEEDBACK_LRA, 0)) || (r = update_reg(REG_CONTROL2, CONTROL2_BIDIR, CONTROL2_BIDIR)) ||
	    (r = update_reg(REG_CONTROL3, CONTROL3_ERM_OPEN | CONTROL3_LRA_OPEN | CONTROL3_UNSIGNED, 0)) ||
	    (r = write_reg(REG_RTP, 0x7f))) {
		log_printf("haptics: setup failed (%d)\n", r);
		return false;
	}
	uint8_t cal = 0, comp = 0;
	r = calibrate(&cal);
	read_reg(REG_CAL_COMP, &comp);
	read_reg(REG_CAL_BEMF, &bemf);
	write_reg(REG_MODE, MODE_STANDBY);
	write_reg(REG_CLAMP, 0x96);
	log_printf("haptics: drv2605 status %02x, calibration %d status %02x comp %02x bemf %02x\n", status, r, cal, comp,
		   bemf);
	ready = !r && !(cal & STATUS_DIAG);
	return ready;
}

static void motor(bool on)
{
	write_reg(REG_MODE, on ? MODE_RTP : MODE_STANDBY);
}

void haptics_play(const uint16_t *on_off_ms, unsigned n)
{
	if (!ready)
		return;
	if (pattern)
		motor(false);
	pattern = n ? on_off_ms : NULL;
	steps = n;
	step = 0;
	step_t = time_ms();
	if (pattern)
		motor(true);
}

void haptics_poll(void)
{
	if (!pattern || time_ms() - step_t < pattern[step])
		return;
	step_t = time_ms();
	if (++step == steps) {
		pattern = NULL;
		motor(false);
		return;
	}
	motor(!(step & 1));
}

bool haptics_busy(void)
{
	return pattern != NULL;
}
