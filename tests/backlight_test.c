/* The LM3535's brightness curve and its driver. */
#include <stdio.h>
#include "display/backlight_lm3535.c"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const struct board test_board;
const struct board *board = &test_board;

/* The last value written to each register and the enable line. */
static uint8_t regs[256];
static unsigned enable_line;
void tlmm_set(unsigned n, unsigned v) { enable_line = v; }
void tlmm_config(unsigned n, unsigned func, unsigned dir, unsigned pull, unsigned drive_ma) {}
void delay_ms(uint32_t ms) {}
void log_printf(const char *fmt, ...) {}
int i2c_qup_init(unsigned qup, unsigned sda, unsigned scl, unsigned func, uint32_t bus_hz) { return 0; }
int i2c_qup_xfer(unsigned qup, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	if (wn == 2)
		regs[w[0]] = w[1];
	return 0;
}

static void driver_test(void)
{
	const struct backlight_driver *d = &backlight_lm3535;
	CHECK(d->init());
	/* A code goes to the chip as it is, under the curve's lowest step. */
	d->set_code(36);
	CHECK(regs[REG_BRIGHT_A] == 36);
	d->set(100);
	CHECK(regs[REG_BRIGHT_A] == lm3535_code(100));
	/* A code comes back from dark with the banks on. */
	d->set(0);
	CHECK(regs[REG_BRIGHT_A] == 0 && regs[REG_ENABLE] == 0 && !enable_line);
	d->set_code(36);
	CHECK(regs[REG_BRIGHT_A] == 36 && regs[REG_ENABLE] == BANKS_ON && enable_line);
}

int main(void)
{
	driver_test();
	CHECK(lm3535_code(0) == 0);
	CHECK(lm3535_code(4) == 0);
	CHECK(lm3535_code(5) == LM3535_DIM);
	CHECK(lm3535_code(9) == LM3535_DIM);
	CHECK(lm3535_code(10) == 67);
	CHECK(lm3535_code(255) == 108);
	/* It only goes up and stays in range. */
	for (int l = 11; l < 256; l++) {
		CHECK(lm3535_code((uint8_t)l) >= lm3535_code((uint8_t)(l - 1)));
		CHECK(lm3535_code((uint8_t)l) <= LM3535_MAX);
	}
	puts(failures ? "backlight_test: FAILED" : "backlight_test: ok");
	return failures != 0;
}
