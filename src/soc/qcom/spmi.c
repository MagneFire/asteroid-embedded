/* PMIC arbiter (SPMI master): commands written to CMD, completion polled
 * in STATUS, data through the WDATA/RDATA words. A register address is
 * sid << 16 | peripheral << 8 | offset, as the PMIC documents it.
 * Version 1 (APQ8026) has one channel for this execution environment
 * and takes the whole address in the command. Version 2 (APQ8009W) has
 * a channel per peripheral, found in a table at the core, with writes
 * through one window and reads through another, and only the offset in
 * the command. */
#include <string.h>
#include "asteroid.h"

#define CH SPMI_ARB_CHNL(0)
#define OP_WRITE_LONG	0x00
#define OP_READ_LONG	0x01
#define STATUS_DONE	BIT(0)
#define V2_CHANNELS	128
#define V2_NONE		0xff

static uint32_t version;
static uint8_t v2_channel[16 * 256];

static bool v2(void) { return chip->spmi_chnls; }

void spmi_init(void)
{
	version = readl(SPMI_ARB_VERSION);
	if (!v2())
		return;
	memset(v2_channel, V2_NONE, sizeof v2_channel);
	for (unsigned n = 0; n < V2_CHANNELS; n++) {
		uint32_t r = readl(chip->spmi_core + 0x800 + 4 * n);
		if (r)
			v2_channel[(r >> 8) & 0xfff] = (uint8_t)n;
	}
}

uint32_t spmi_version(void) { return version; }

static int v2_lookup(uint32_t addr)
{
	uint8_t c = v2_channel[(addr >> 8) & 0xfff];
	return c == V2_NONE ? -1 : c;
}

/* The channel's registers for a write or a read, 0 if the peripheral
 * has none. */
static uint32_t channel(uint32_t addr, bool read)
{
	if (!v2())
		return CH;
	int c = v2_lookup(addr);
	return c < 0 ? 0 : (read ? chip->spmi_obsrvr : chip->spmi_chnls) + 0x8000 * (uint32_t)c;
}

static uint32_t cmd_word(uint32_t op, uint32_t addr, unsigned n)
{
	if (v2())
		return op << 27 | (addr & 0xff) << 4 | (n - 1);
	return op << 27 | ((addr >> 16) & 0xf) << 20 | ((addr >> 8) & 0xff) << 12 |
	       (addr & 0xff) << 4 | (n - 1);
}

static int wait_done(uint32_t ch)
{
	uint32_t st;
	for (int i = 0; i < 100000; i++) {
		st = readl(ch + SPMI_STATUS);
		if (st)
			return st == STATUS_DONE ? 0 : (int)st;
	}
	return -1;
}

int spmi_write(uint32_t addr, const uint8_t *buf, unsigned n)
{
	uint32_t w[2] = { 0, 0 }, ch = channel(addr, false);
	if (n == 0 || n > 8)
		return -2;
	if (!ch)
		return -3;
	for (unsigned i = 0; i < n; i++)
		w[i / 4] |= (uint32_t)buf[i] << (8 * (i % 4));
	writel(0, ch + SPMI_CONFIG);
	writel(w[0], ch + SPMI_WDATA0);
	if (n > 4)
		writel(w[1], ch + SPMI_WDATA1);
	writel(cmd_word(OP_WRITE_LONG, addr, n), ch + SPMI_CMD);
	return wait_done(ch);
}

int spmi_read(uint32_t addr, uint8_t *buf, unsigned n)
{
	uint32_t r[3], ch = channel(addr, true);
	int ret;
	if (n == 0 || n > 8)
		return -2;
	if (!ch)
		return -3;
	writel(0, ch + SPMI_CONFIG);
	writel(cmd_word(OP_READ_LONG, addr, n), ch + SPMI_CMD);
	ret = wait_done(ch);
	if (ret)
		return ret;
	r[0] = readl(ch + SPMI_RDATA0);
	r[1] = n > 4 ? readl(ch + SPMI_RDATA1) : 0;
	r[2] = 0;
	for (unsigned i = 0; i < n; i++)
		buf[i] = (uint8_t)(r[i / 4] >> (8 * (i % 4)));
	return 0;
}

/* Peripheral interrupts: the arbiter maps each peripheral (sid << 8 |
 * peripheral) to an APID, collects the interrupt messages per APID and
 * raises one level interrupt for this execution environment while any
 * enabled APID has one pending. Version 1 finds the APID through a
 * binary decision tree in its config block; on version 2 it is the
 * peripheral's channel, and each APID has its own page of registers. */
#define SPMI_MAPPING(n)		(SPMI_CNFG + 0xb00 + 4 * (n))
#define SPMI_ACC_ENABLE(apid)	(SPMI_INTR + (v2() ? 0x1000 * (apid) : 0x200 + 4 * (apid)))
#define SPMI_IRQ_STATUS(apid)	(SPMI_INTR + (v2() ? 0x1000 * (apid) + 4 : 0x600 + 4 * (apid)))
#define SPMI_IRQ_CLEAR(apid)	(SPMI_INTR + (v2() ? 0x1000 * (apid) + 8 : 0xa00 + 4 * (apid)))

int spmi_apid(uint32_t sid, uint32_t periph)
{
	uint32_t ppid = (sid & 0xf) << 8 | (periph & 0xff), index = 0;
	if (v2())
		return v2_lookup(ppid << 8);
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
