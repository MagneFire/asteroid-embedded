/* PixArt PAJ9124U1 optical track sensor under narwhal's crown, on SPI
 * mode 3 with a chip select of our own. A read is the address, a 10 us
 * pause, then the value; a write sets bit 7 of the address. The vendor
 * powers VDD, holds chip select for 1 ms, writes its start-up tables,
 * then powers the LED supply VLD. The motion line goes low while there is
 * motion to read; each turn of the crown is X counts, read until the
 * motion bit clears. Bit 3 of CONFIG puts it in low power. */
#include "asteroid.h"

#define PINS		(board->crown_pins)
#define SPI_HZ		2000000
#define READ_GAP_US	10
#define VDD_MS		10
#define CS_START_MS	1

#define REG_PID		0x00
#define PID		0x30
#define REG_MOTION	0x02
#define REG_DELTA_X	0x03
#define REG_DELTA_Y	0x04
#define REG_CONFIG	0x06
#define REG_WRITE_PROTECT	0x09
#define REG_BANK	0x7f
#define MOTION		0x80
#define CONFIG_LOW_POWER	0x08
#define CONFIG_MASK	0x88
#define UNPROTECT	0x5a
#define WRITE_RETRIES	5

/* The vendor's start-up writes: bank 0, then bank 1. */
static const uint8_t bank0[][2] = {
	{ 0x05, 0xa0 }, { 0x0d, 0x05 }, { 0x0e, 0x0a }, { 0x19, 0x04 }, { 0x2b, 0x6d }, { 0x4b, 0x04 }, { 0x5c, 0xd7 },
};
static const uint8_t bank1[][2] = {
	{ 0x09, 0x22 }, { 0x2a, 0x03 }, { 0x30, 0x4c }, { 0x33, 0x90 }, { 0x36, 0xcc }, { 0x37, 0x51 }, { 0x38, 0x01 },
	{ 0x3a, 0x7a }, { 0x40, 0x38 }, { 0x41, 0x33 }, { 0x42, 0x4f }, { 0x43, 0x83 }, { 0x44, 0x4f }, { 0x45, 0x80 },
	{ 0x46, 0x23 }, { 0x47, 0x49 }, { 0x48, 0xc3 }, { 0x49, 0x49 }, { 0x4a, 0xc0 }, { 0x52, 0x00 }, { 0x61, 0x80 },
	{ 0x62, 0x51 }, { 0x67, 0x53 }, { 0x68, 0x13 }, { 0x6c, 0x10 }, { 0x6f, 0xf6 }, { 0x71, 0x28 }, { 0x72, 0x28 },
	{ 0x79, 0x08 },
};

static bool ready, asleep;
static volatile bool pending;
static int32_t counts;
static uint32_t irqs, errors;

static int rd(uint8_t reg, uint8_t *v)
{
	gpio_set(PINS.cs, 0);
	int r = spi_xfer(PINS.spi, &reg, NULL, 1);
	delay_us(READ_GAP_US);
	if (!r)
		r = spi_xfer(PINS.spi, NULL, v, 1);
	gpio_set(PINS.cs, 1);
	if (r)
		errors++;
	return r;
}

static int wr(uint8_t reg, uint8_t v)
{
	uint8_t tx[2] = { (uint8_t)(0x80 | reg), v };
	gpio_set(PINS.cs, 0);
	int r = spi_xfer(PINS.spi, tx, NULL, 2);
	gpio_set(PINS.cs, 1);
	if (r)
		errors++;
	return r;
}

static bool wr_check(uint8_t reg, uint8_t v)
{
	for (unsigned i = 0; i <= WRITE_RETRIES; i++) {
		uint8_t back = 0;
		if (wr(reg, v) || rd(reg, &back))
			return false;
		if (back == v)
			return true;
	}
	return false;
}

static bool start_up(void)
{
	if (wr(REG_BANK, 0) || !wr_check(REG_WRITE_PROTECT, UNPROTECT))
		return false;
	for (unsigned i = 0; i < ARRAY_SIZE(bank0); i++)
		if (!wr_check(bank0[i][0], bank0[i][1]))
			return false;
	if (wr(REG_BANK, 1))
		return false;
	for (unsigned i = 0; i < ARRAY_SIZE(bank1); i++)
		if (!wr_check(bank1[i][0], bank1[i][1]))
			return false;
	return !wr(REG_BANK, 0) && wr_check(REG_WRITE_PROTECT, 0);
}

static void motion(unsigned pin)
{
	gpio_irq_enable(PINS.irq, false);
	irqs++;
	pending = true;
}

static bool paj9124_init(void)
{
	gpio_set(PINS.cs, 1);
	gpio_config(PINS.cs, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_config(PINS.irq, 0, GPIO_INPUT, GPIO_PULL_UP, 2);
	if (regulator_on(PINS.vdd_ldo, PINS.vdd_uv))
		log_printf("crown: l%u on not acknowledged\n", PINS.vdd_ldo);
	delay_ms(VDD_MS);
	const uint8_t pins[3] = { PINS.mosi, PINS.miso, PINS.clk };
	if (spi_init(PINS.spi, pins, PINS.func, 3, SPI_HZ)) {
		log_printf("crown: no spi\n");
		return false;
	}
	gpio_set(PINS.cs, 0);
	delay_ms(CS_START_MS);
	gpio_set(PINS.cs, 1);
	uint8_t pid = 0;
	if (rd(REG_PID, &pid) || pid != PID) {
		log_printf("crown: paj9124u1 not found (pid %02x)\n", pid);
		return false;
	}
	if (!start_up()) {
		log_printf("crown: paj9124u1 start-up writes failed\n");
		return false;
	}
	if (regulator_on(PINS.vld_ldo, PINS.vld_uv))
		log_printf("crown: l%u on not acknowledged\n", PINS.vld_ldo);
	ready = true;
	gpio_irq_config(PINS.irq, GPIO_IRQ_LEVEL_LOW, motion);
	gpio_irq_enable(PINS.irq, true);
	log_printf("crown: paj9124u1\n");
	return true;
}

static int32_t paj9124_poll(void)
{
	if (!ready || !pending)
		return 0;
	pending = false;
	uint8_t m = MOTION;
	for (unsigned i = 0; i < 8 && (m & MOTION); i++) {
		uint8_t x = 0, y = 0;
		if (rd(REG_DELTA_X, &x) || rd(REG_DELTA_Y, &y) || rd(REG_MOTION, &m))
			break;
		counts += (int8_t)x;
	}
	int32_t c = counts;
	counts = 0;
	gpio_irq_ack(PINS.irq);
	gpio_irq_enable(PINS.irq, !asleep);
	return asleep ? 0 : c;
}

static void config_low_power(bool on)
{
	uint8_t v = 0;
	if (!rd(REG_CONFIG, &v))
		wr(REG_CONFIG, (uint8_t)((v & ~CONFIG_MASK) | (on ? CONFIG_LOW_POWER : 0)));
}

/* As the vendor suspends it: the line masked and the sensor in low power. */
static void paj9124_sleep(bool on)
{
	if (!ready || on == asleep)
		return;
	asleep = on;
	gpio_irq_enable(PINS.irq, false);
	config_low_power(on);
	pending = false;
	counts = 0;
	if (!on) {
		gpio_irq_ack(PINS.irq);
		gpio_irq_enable(PINS.irq, true);
	}
}

static void paj9124_stats(uint32_t *n_irqs, uint32_t *n_errors)
{
	*n_irqs = irqs;
	*n_errors = errors;
}

const struct crown_driver crown_paj9124 = { .init = paj9124_init, .poll = paj9124_poll, .sleep = paj9124_sleep,
					    .stats = paj9124_stats };
