/* PMIC arbiter (SPMI master), version 1 register interface: one channel for
 * this execution environment, commands written to CMD, completion polled
 * in STATUS, data through the WDATA/RDATA words. A register address is
 * sid << 16 | peripheral << 8 | offset, as the PMIC documents it. */
#include "asteroid.h"

#define CH SPMI_ARB_CHNL(0)
#define OP_WRITE_LONG	0x00
#define OP_READ_LONG	0x01
#define STATUS_DONE	BIT(0)

static uint32_t version;

void spmi_init(void)
{
	version = readl(SPMI_ARB_VERSION);
}

uint32_t spmi_version(void) { return version; }

static uint32_t cmd_word(uint32_t op, uint32_t addr, unsigned n)
{
	return op << 27 | ((addr >> 16) & 0xf) << 20 | ((addr >> 8) & 0xff) << 12 |
	       (addr & 0xff) << 4 | (n - 1);
}

static int wait_done(void)
{
	uint32_t st;
	for (int i = 0; i < 100000; i++) {
		st = readl(CH + SPMI_STATUS);
		if (st)
			return st == STATUS_DONE ? 0 : (int)st;
	}
	return -1;
}

int spmi_write(uint32_t addr, const uint8_t *buf, unsigned n)
{
	uint32_t w[2] = { 0, 0 };
	if (n == 0 || n > 8)
		return -2;
	for (unsigned i = 0; i < n; i++)
		w[i / 4] |= (uint32_t)buf[i] << (8 * (i % 4));
	writel(0, CH + SPMI_CONFIG);
	writel(w[0], CH + SPMI_WDATA0);
	if (n > 4)
		writel(w[1], CH + SPMI_WDATA1);
	writel(cmd_word(OP_WRITE_LONG, addr, n), CH + SPMI_CMD);
	return wait_done();
}

int spmi_read(uint32_t addr, uint8_t *buf, unsigned n)
{
	uint32_t r[3];
	int ret;
	if (n == 0 || n > 8)
		return -2;
	writel(0, CH + SPMI_CONFIG);
	writel(cmd_word(OP_READ_LONG, addr, n), CH + SPMI_CMD);
	ret = wait_done();
	if (ret)
		return ret;
	r[0] = readl(CH + SPMI_RDATA0);
	r[1] = n > 4 ? readl(CH + SPMI_RDATA1) : 0;
	r[2] = 0;
	for (unsigned i = 0; i < n; i++)
		buf[i] = (uint8_t)(r[i / 4] >> (8 * (i % 4)));
	return 0;
}

/* Peripheral interrupts: the arbiter maps each peripheral (sid << 8 |
 * peripheral) to an APID through a binary decision tree in its config
 * block, collects the interrupt messages per APID and raises one level
 * interrupt for this execution environment while any enabled APID has
 * one pending. */
#define SPMI_MAPPING(n)		(SPMI_CNFG + 0xb00 + 4 * (n))
#define SPMI_ACC_STATUS(n)	(SPMI_INTR + 4 * (n))
#define SPMI_ACC_ENABLE(apid)	(SPMI_INTR + 0x200 + 4 * (apid))
#define SPMI_IRQ_STATUS(apid)	(SPMI_INTR + 0x600 + 4 * (apid))
#define SPMI_IRQ_CLEAR(apid)	(SPMI_INTR + 0xa00 + 4 * (apid))

int spmi_apid(uint32_t sid, uint32_t periph)
{
	uint32_t ppid = (sid & 0xf) << 8 | (periph & 0xff), index = 0;
	for (int depth = 0; depth < 16; depth++) {
		uint32_t m = readl(SPMI_MAPPING(index));
		bool one = ppid & BIT((m >> 18) & 0xf);
		bool next = one ? (m >> 8) & 1 : (m >> 17) & 1;
		uint32_t result = one ? m & 0xff : (m >> 9) & 0xff;
		if (!next)
			return (int)result;
		index = result;
	}
	return -1;
}

void spmi_irq_enable(int apid, bool on)
{
	writel(on, SPMI_ACC_ENABLE(apid));
}

uint32_t spmi_irq_status(int apid)
{
	return readl(SPMI_IRQ_STATUS(apid));
}

void spmi_irq_clear(int apid, uint32_t bits)
{
	writel(bits, SPMI_IRQ_CLEAR(apid));
}

uint8_t pmic_read(uint32_t addr)
{
	uint8_t v = 0;
	spmi_read(addr, &v, 1);
	return v;
}

void pmic_write(uint32_t addr, uint8_t val)
{
	spmi_write(addr, &val, 1);
}
