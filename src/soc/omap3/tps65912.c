/* The TPS65912 power management chip on McSPI1: its supplies and the
 * power key. A register access is one 24-bit word, chip select high:
 * the write bit, the address, seven bits of nothing and the data. Its
 * interrupt line comes in on gpio 0, high while a status bit that is
 * not masked is set; the handler only masks the line, as SPI belongs
 * to the main loop, which reads the status and lets it through again.
 * The power key pulls the chip's GPIO3 low. */
#include "asteroid.h"

#define SPI_SYSCONFIG	(MCSPI1_BASE + 0x10)
#define SPI_SYSSTATUS	(MCSPI1_BASE + 0x14)
#define SPI_MODULCTRL	(MCSPI1_BASE + 0x28)
#define SPI_CHCONF	(MCSPI1_BASE + 0x2c)
#define SPI_CHSTAT	(MCSPI1_BASE + 0x30)
#define SPI_CHCTRL	(MCSPI1_BASE + 0x34)
#define SPI_TX		(MCSPI1_BASE + 0x38)
#define SPI_RX		(MCSPI1_BASE + 0x3c)
#define CHCONF_CLKD(n)	((n) << 2)		/* 48 MHz / 2^n */
#define CHCONF_WL(bits)	(((bits) - 1) << 7)
#define CHCONF_DPE0	BIT(16)			/* no transmission on the receive line */
#define CHCONF_FORCE	BIT(20)
#define CHSTAT_RXS	BIT(0)
#define CHSTAT_TXS	BIT(1)
/* 12 MHz, 24 bits, mode 0, chip select active high. */
#define CHCONF		(CHCONF_CLKD(2) | CHCONF_WL(24) | CHCONF_DPE0)

#define DCDC1_AVS	0x05
#define LDO1_OP		0x10
#define LDO5		0x1c
#define INT_STS		0x39
#define INT_MSK		0x3a
#define INT_STS2	0x3b
#define INT_MSK2	0x3c
#define INT_MSK3	0x3e
#define INT_MSK4	0x40
#define GPIO3		0x43
#define VERNUM		0x64
#define REG_ENABLED	BIT(7)
#define OP_SELREG	BIT(6)			/* the voltage is in the AVS register */
#define VSEL_MASK	0x3f
#define GPIO_STS	BIT(1)
#define INT2_GPIO3	(BIT(2) | BIT(3))	/* rising and falling */

#define GPIO_PMIC_IRQ	0

static bool ready;
static volatile bool key_irq;

static bool spi_wait(uint32_t bit)
{
	uint32_t t0 = time_us();
	while (!(readl(SPI_CHSTAT) & bit))
		if (time_us() - t0 > 10000)
			return false;
	return true;
}

static int spi_word(uint32_t tx, uint32_t *rx)
{
	uint32_t flags = irq_save();
	int r = -1;
	writel(CHCONF | CHCONF_FORCE, SPI_CHCONF);
	writel(1, SPI_CHCTRL);
	if (spi_wait(CHSTAT_TXS)) {
		writel(tx, SPI_TX);
		if (spi_wait(CHSTAT_RXS)) {
			*rx = readl(SPI_RX);
			r = 0;
		}
	}
	writel(0, SPI_CHCTRL);
	writel(CHCONF, SPI_CHCONF);
	irq_restore(flags);
	return r;
}

uint8_t pmic_read(uint32_t addr)
{
	uint32_t rx = 0;
	if (!ready || spi_word((addr & 0xff) << 15, &rx))
		return 0;
	return (uint8_t)rx;
}

void pmic_write(uint32_t addr, uint8_t val)
{
	uint32_t rx;
	if (ready)
		spi_word(BIT(23) | (addr & 0xff) << 15 | val, &rx);
}

/* The 6-bit voltage code of an LDO: 25 mV steps from 0.8 V, 50 mV from
 * 1.6 V, 100 mV from 3.0 V. */
static uint8_t ldo_vsel(uint32_t uv)
{
	if (uv <= 1600000)
		return (uint8_t)(uv <= 800000 ? 0 : (uv - 800000) / 25000);
	if (uv <= 3000000)
		return (uint8_t)(32 + (uv - 1600000) / 50000);
	uint32_t sel = 60 + (uv - 3000000) / 100000;
	return (uint8_t)(sel > 63 ? 63 : sel);
}

/* LDO1 to 4 have three registers each, with the voltage in the one the
 * first names and the enable bit in the second; LDO5 to 10 have one. */
int regulator_on(uint32_t id, uint32_t uv)
{
	if (!ready || id < 1 || id > 10)
		return -1;
	uint8_t sel = ldo_vsel(uv);
	if (id >= 5) {
		uint8_t reg = (uint8_t)(LDO5 + id - 5);
		pmic_write(reg, (pmic_read(reg) & ~(REG_ENABLED | VSEL_MASK)) | REG_ENABLED | sel);
		return pmic_read(reg) & REG_ENABLED ? 0 : -1;
	}
	uint8_t op = (uint8_t)(LDO1_OP + (id - 1) * 3), avs = (uint8_t)(op + 1);
	uint8_t vreg = pmic_read(op) & OP_SELREG ? avs : op;
	pmic_write(vreg, (pmic_read(vreg) & ~VSEL_MASK) | sel);
	pmic_write(avs, pmic_read(avs) | REG_ENABLED);
	return pmic_read(avs) & REG_ENABLED ? 0 : -1;
}

int regulator_off(uint32_t id)
{
	if (!ready || id < 1 || id > 10)
		return -1;
	uint8_t reg = id >= 5 ? (uint8_t)(LDO5 + id - 5) : (uint8_t)(LDO1_OP + (id - 1) * 3 + 1);
	pmic_write(reg, pmic_read(reg) & ~REG_ENABLED);
	return 0;
}

bool power_key(void)
{
	return ready && !(pmic_read(GPIO3) & GPIO_STS);
}

static void pmic_irq(unsigned pin)
{
	gpio_irq_enable(GPIO_PMIC_IRQ, false);
	key_irq = true;
}

void power_key_irq_init(void)
{
	if (!ready)
		return;
	pmic_write(INT_MSK, 0xff);
	pmic_write(INT_MSK2, (uint8_t)~INT2_GPIO3);
	pmic_write(INT_MSK3, 0xff);
	pmic_write(INT_MSK4, 0xff);
	pmic_write(INT_STS, pmic_read(INT_STS));
	pmic_write(INT_STS2, pmic_read(INT_STS2));
	gpio_config(GPIO_PMIC_IRQ, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	gpio_irq_config(GPIO_PMIC_IRQ, GPIO_IRQ_LEVEL_HIGH, pmic_irq);
	gpio_irq_enable(GPIO_PMIC_IRQ, true);
}

bool power_key_irq(void)
{
	if (!key_irq)
		return false;
	key_irq = false;
	pmic_write(INT_STS, pmic_read(INT_STS));
	pmic_write(INT_STS2, pmic_read(INT_STS2));
	gpio_irq_enable(GPIO_PMIC_IRQ, true);
	return true;
}

/* What the vendor's driver writes when it starts. */
static const uint8_t setup[][2] = {
	{ 0x06, 0x7f },	/* DCDC1 from 0.7 V in 12.5 mV steps */
	{ 0x25, 0xd1 },	/* sleep allowed, interrupt line push-pull and active high */
	{ 0x26, 0x49 },	/* DCDC1 and DCDC4 take their voltage over the SoC's own bus */
	{ 0x29, 0x10 },	/* LDO5 goes off in sleep */
	{ 0x2a, 0x24 },	/* and so do DCDC1 and DCDC4 */
	{ 0x2d, 0x88 },	/* LDO4 and LDO8 discharge when off */
	{ 0x17, 0xc0 },	/* LDO3 in its low power mode */
	{ 0x19, 0x64 },	/* LDO4 switched by its second register, 1.8 V */
	{ 0x1a, 0x24 },	/* and off */
	{ 0x1b, 0x3f },
	{ 0x21, 0xf2 },	/* LDO10 in its low power mode */
	{ 0x62, 0x00 },	/* load switch off */
};

bool tps65912_init(void)
{
	clock_enable(CM_CORE, CORE_MCSPI1, true);
	writel(BIT(1), SPI_SYSCONFIG);
	for (unsigned i = 0; i < 100000 && !(readl(SPI_SYSSTATUS) & 1); i++)
		;
	/* Master, one channel, the chip select driven by hand. */
	writel(1, SPI_MODULCTRL);
	writel(CHCONF, SPI_CHCONF);
	ready = true;
	uint8_t ver = pmic_read(VERNUM), dcdc1 = pmic_read(DCDC1_AVS);
	if (ver == 0xff && dcdc1 == 0xff) {
		log_printf("pmic: no answer (%02x)\n", ver);
		ready = false;
		return false;
	}
	for (unsigned i = 0; i < ARRAY_SIZE(setup); i++)
		pmic_write(setup[i][0], setup[i][1]);
	log_printf("pmic: tps65912 version %02x, dcdc1 %02x, key %s\n", ver, dcdc1, power_key() ? "down" : "up");
	return true;
}
