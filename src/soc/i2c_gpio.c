/* I2C master bit-banged on two TLMM pins, open-drain style: a line is
 * driven low by making the pin an output at 0 and released by making it
 * an input, the external pull-ups do the rest. About 100 kHz. */
#include "asteroid.h"

static unsigned sda_pin, scl_pin;

static void release(unsigned pin) { tlmm_config(pin, 0, TLMM_INPUT, TLMM_PULL_NONE, 2); }
static void drive_low(unsigned pin)
{
	tlmm_set(pin, 0);
	tlmm_config(pin, 0, TLMM_OUTPUT, TLMM_PULL_NONE, 2);
}
static void half(void) { delay_us(4); }

static void scl_high(void)
{
	release(scl_pin);
	for (int i = 0; i < 1000 && !tlmm_get(scl_pin); i++)
		delay_us(1);
}

static void start(void)
{
	release(sda_pin);
	scl_high();
	half();
	drive_low(sda_pin);
	half();
	drive_low(scl_pin);
	half();
}

static void stop(void)
{
	drive_low(sda_pin);
	half();
	scl_high();
	half();
	release(sda_pin);
	half();
}

static bool write_byte(uint8_t b)
{
	for (int i = 7; i >= 0; i--) {
		if (b & (1 << i))
			release(sda_pin);
		else
			drive_low(sda_pin);
		half();
		scl_high();
		half();
		drive_low(scl_pin);
	}
	release(sda_pin);
	half();
	scl_high();
	bool ack = !tlmm_get(sda_pin);
	half();
	drive_low(scl_pin);
	half();
	return ack;
}

static uint8_t read_byte(bool ack)
{
	uint8_t b = 0;
	release(sda_pin);
	for (int i = 7; i >= 0; i--) {
		half();
		scl_high();
		if (tlmm_get(sda_pin))
			b |= 1 << i;
		half();
		drive_low(scl_pin);
	}
	if (ack)
		drive_low(sda_pin);
	else
		release(sda_pin);
	half();
	scl_high();
	half();
	drive_low(scl_pin);
	release(sda_pin);
	half();
	return b;
}

void i2c_gpio_init(unsigned sda, unsigned scl)
{
	sda_pin = sda;
	scl_pin = scl;
	release(sda_pin);
	release(scl_pin);
	for (int i = 0; i < 9 && !tlmm_get(sda_pin); i++) {
		drive_low(scl_pin);
		half();
		scl_high();
		half();
	}
	stop();
}

int i2c_gpio_xfer(uint8_t addr, const uint8_t *wbuf, unsigned wn, uint8_t *rbuf, unsigned rn)
{
	int ret = 0;
	if (wn) {
		start();
		if (!write_byte((uint8_t)(addr << 1))) {
			ret = -1;
			goto out;
		}
		for (unsigned i = 0; i < wn; i++)
			if (!write_byte(wbuf[i])) {
				ret = -2;
				goto out;
			}
	}
	if (rn) {
		start();
		if (!write_byte((uint8_t)(addr << 1 | 1))) {
			ret = -3;
			goto out;
		}
		for (unsigned i = 0; i < rn; i++)
			rbuf[i] = read_byte(i + 1 < rn);
	}
out:
	stop();
	return ret;
}
