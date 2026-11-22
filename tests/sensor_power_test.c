/* beluga's AK09918 and the LSM6DSO's gyroscope turned on and off, also
 * when the controller reports a write as failed that the chip took. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "board/board.h"
#include "sensors/ak09918.h"
#include "sensors/hub.h"
#include "sensors/lsm6dso.h"
#include "sensors/raise.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define MAG	0x0c
#define LSM	0x6a
#define CNTL2	0x31
#define CTRL2_G	0x11

static const struct board test_board = {
	.chips_off = { .i2c = { .bus = 0, .addr = MAG } },
	.hub_pins = { .i2c = { .bus = 1, .addr = LSM } },
};
const struct board *board = &test_board;

/* Each chip's registers. The write to fail_reg after fail_skip others
 * to it fails, once, and with lands set it still reaches the register. */
static uint8_t regs[2][0x80];
static int fail_reg = -1;
static unsigned fail_skip;
static bool fail_lands;

static void fail(int reg, unsigned skip, bool lands)
{
	fail_reg = reg;
	fail_skip = skip;
	fail_lands = lands;
}

static uint8_t *chip(uint8_t addr) { return addr == MAG ? regs[0] : addr == LSM ? regs[1] : NULL; }

int i2c_xfer(unsigned bus, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	uint8_t *c = chip(addr);
	if (!c || !wn)
		return -6;
	if (wn == 2 && w[0] == fail_reg && !fail_skip--) {
		fail_reg = -1;
		if (fail_lands)
			c[w[0]] = w[1];
		return -5;
	}
	if (wn == 2)
		c[w[0]] = w[1];
	for (unsigned i = 0; i < rn; i++)
		r[i] = c[(w[0] + i) & 0x7f];
	return 0;
}
int i2c_init(unsigned bus, unsigned sda, unsigned scl, unsigned func, uint32_t bus_hz) { return 0; }
uint32_t time_ms(void) { return 0; }
void delay_us(uint32_t us) {}
void delay_ms(uint32_t ms) {}
void log_printf(const char *fmt, ...) {}
void gpio_config(unsigned n, unsigned func, unsigned dir, unsigned pull, unsigned drive_ma) {}
unsigned gpio_get(unsigned n) { return 0; }
void gpio_irq_config(unsigned n, unsigned type, void (*fn)(unsigned n)) {}
void gpio_irq_enable(unsigned n, bool on) {}
void raise_init(struct raise *r) {}
int raise_feed(struct raise *r, const float a[3], bool screen_on) { return 0; }
void raise_screen(struct raise *r, bool on) {}
unsigned raise_repeats(int32_t elapsed_ms) { return 0; }
void afe4900_on(bool on) {}
uint32_t afe4900_rate(uint8_t *status, unsigned *readings) { return 0; }
void afe4900_poll(void) {}
bool afe4900_can_sleep(void) { return true; }
void afe4900_status(void (*say)(const char *fmt, ...)) {}
void sns_compass(bool on) {}
int sns_heading(float *degrees) { return -1; }

static void magnetometer(void)
{
	regs[0][0x00] = 0x48, regs[0][0x01] = 0x0c;
	CHECK(ak09918_on(20) && regs[0][CNTL2] == 0x08);
	CHECK(ak09918_on(0) && regs[0][CNTL2] == 0);
	/* The mode, after power-down, taken but reported as failed: off
	 * writes power-down all the same. */
	fail(CNTL2, 1, true);
	CHECK(!ak09918_on(20) && regs[0][CNTL2] == 0x08);
	CHECK(ak09918_on(0) && regs[0][CNTL2] == 0);
	/* Power-down not taken: asked again until it is. */
	CHECK(ak09918_on(20) && regs[0][CNTL2] == 0x08);
	fail(CNTL2, 0, false);
	CHECK(!ak09918_on(0) && regs[0][CNTL2] == 0x08);
	CHECK(ak09918_on(0) && regs[0][CNTL2] == 0);
	CHECK(ak09918_on(0));
}

static void gyroscope(void)
{
	regs[1][0x0f] = 0x6c;
	CHECK(hub_lsm6dso.init());
	CHECK(lsm6dso_gyro_on(20) && regs[1][CTRL2_G] == (3 << 4 | 0x0c));
	CHECK(lsm6dso_gyro_on(0) && regs[1][CTRL2_G] == 0);
	fail(CTRL2_G, 0, true);
	CHECK(!lsm6dso_gyro_on(20) && regs[1][CTRL2_G] == (3 << 4 | 0x0c));
	CHECK(lsm6dso_gyro_on(0) && regs[1][CTRL2_G] == 0);
	CHECK(lsm6dso_gyro_on(100) && regs[1][CTRL2_G] == (1 << 4 | 0x0c));
	fail(CTRL2_G, 0, false);
	CHECK(!lsm6dso_gyro_on(0) && regs[1][CTRL2_G] == (1 << 4 | 0x0c));
	CHECK(lsm6dso_gyro_on(0) && regs[1][CTRL2_G] == 0);
	CHECK(lsm6dso_gyro_on(0));
}

int main(void)
{
	magnetometer();
	gyroscope();
	if (failures)
		return 1;
	printf("sensor_power_test: ok\n");
	return 0;
}
