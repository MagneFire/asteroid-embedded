/* ST's STC3117 algorithm against a register file standing in for the chip. */
#include <stdio.h>
#include <string.h>
#include "soc/stc3117.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static uint8_t regs[0x80];
static unsigned writes_to[0x80];
static bool fail;

static int rd(uint8_t reg, uint8_t *buf, unsigned n)
{
	if (fail)
		return -1;
	memcpy(buf, regs + reg, n);
	return 0;
}

static int wr(uint8_t reg, const uint8_t *buf, unsigned n)
{
	if (fail)
		return -1;
	memcpy(regs + reg, buf, n);
	for (unsigned i = 0; i < n; i++)
		writes_to[reg + i]++;
	return 0;
}

static const struct stc3117_io io = { rd, wr };

/* swift's cell, as in its board file. */
static const struct stc3117_cell cell = {
	.cnom_mah = 340, .rsense_mohm = 10, .rint_mohm = 501, .cc_cnf = 66, .vm_cnf = 184,
	.alm_soc = 10, .alm_mv = 3600, .relax_ma = 17,
	.derating = { 0, 0, 0, 10, 37, 37, 37 },
	.ocv_mv = { 3400, 3605, 3672, 3687, 3704, 3734, 3755, 3779, 3818, 3866, 3947, 3999, 4053, 4160, 4226, 4390 },
	.soc = { 0x00, 0x06, 0x0c, 0x14, 0x1e, 0x28, 0x32, 0x3c, 0x50, 0x64, 0x78, 0x82, 0x8c, 0xa0, 0xb4, 0xc8 },
};

static unsigned word(unsigned reg) { return regs[reg] | regs[reg + 1] << 8; }

static void set_word(unsigned reg, unsigned v)
{
	regs[reg] = (uint8_t)v;
	regs[reg + 1] = (uint8_t)(v >> 8);
}

/* A chip just out of power on: id, a state of charge, a voltage and a
 * temperature, nothing in its RAM. */
static void power_on(void)
{
	memset(regs, 0, sizeof regs);
	memset(writes_to, 0, sizeof writes_to);
	regs[0x18] = 0x16;
	regs[0x01] = 0x10;
	set_word(0x02, 80 * 512);
	set_word(0x08, 1745);	/* 1745 counts of 2.2 mV, rounded down */
	regs[0x0a] = 23;
	set_word(0x0d, 0x1b40);
}

int main(void)
{
	struct stc3117 g;
	struct stc3117_result r;

	/* The vendor's CRC of an all-zero RAM but its CRC byte is 0. */
	uint8_t z[16] = { 0 };
	CHECK(stc3117_crc8(z, 16) == 0);
	/* Between 25 C (100) and 10 C (160) the VM gain is interpolated. */
	static const int tx[] = { 60, 40, 25, 10, 0, -10, -20 }, ty[] = { 85, 90, 100, 160, 320, 440, 840 };
	CHECK(stc3117_interpolate(23, 7, tx, ty) == 109);
	CHECK(stc3117_interpolate(70, 7, tx, ty) == 85);
	CHECK(stc3117_interpolate(-30, 7, tx, ty) == 840);

	/* No chip. */
	power_on();
	regs[0x18] = 0;
	CHECK(stc3117_start(&g, &cell, &io) < 0);

	/* A fresh start writes the cell's numbers and leaves the chip
	 * running in mixed mode, with a valid RAM in the init state. */
	power_on();
	CHECK(stc3117_start(&g, &cell, &io) == 0 && g.start == 'F');
	CHECK(word(0x30) == 3400 * 100 / 55 && word(0x4e) == 4390 * 100 / 55);
	CHECK(regs[0x50] == 0x00 && regs[0x51] == 0x06 && regs[0x5f] == 0xc8 && regs[0x60] == 0);
	CHECK(word(0x0f) == 66 && word(0x11) == 184);
	CHECK(regs[0x13] == 20 && regs[0x14] == (3600 << 9) / 9011 && regs[0x15] == 17);
	CHECK(regs[0x00] == 0x18 && regs[0x01] == 0x03);
	CHECK(word(0x20) == 0x53a9 && stc3117_crc8(regs + 0x20, 16) == 0 && regs[0x29] == 'I');

	/* Before the first conversion only the state of charge and the
	 * voltage are there. */
	CHECK(stc3117_task(&g, &r) == 0);
	CHECK(r.soc == 800 && r.mv == 3839 && r.ma == 0);

	/* Then it runs: the VM gain follows the temperature, charging is
	 * positive. */
	set_word(0x04, 3);
	set_word(0x06, 255);	/* 255 * 5.88 uV / 10 mOhm = 150 mA */
	set_word(0x0b, 255 * 4);
	regs[0x00] = 0x18;
	CHECK(stc3117_task(&g, &r) == 1);
	CHECK(word(0x11) == 184 * 109 / 100 && regs[0x29] == 'R');
	CHECK(r.ma == 150 && r.decidegrees == 230 && r.soc == 800 && r.mv == 3839);
	CHECK(regs[0x28] == 80 && stc3117_crc8(regs + 0x20, 16) == 0);

	/* A restart of the host with the RAM kept and no reset of the chip
	 * puts the saved state of charge back. */
	set_word(0x02, 10 * 512);
	CHECK(stc3117_start(&g, &cell, &io) == 0 && g.start == 'R');
	CHECK(word(0x02) == 80 * 512);
	CHECK(word(0x0f) == 66 && word(0x11) == 184 * 109 / 100);

	/* After the chip's own reset it starts over from the OCV. */
	regs[0x01] = 0x10;
	CHECK(stc3117_start(&g, &cell, &io) == 0 && g.start == 'P');

	/* An undervoltage resets the chip and gives an error. */
	regs[0x01] = 0x80;
	regs[0x00] = 0x18;
	memset(writes_to, 0, sizeof writes_to);
	CHECK(stc3117_task(&g, &r) == -1);
	CHECK(regs[0x01] == 0x11 && word(0x20) == 0 && writes_to[0x01]);

	/* An empty cell reads 0 % whatever the counter says. */
	power_on();
	CHECK(stc3117_start(&g, &cell, &io) == 0);
	regs[0x00] = 0x18;
	regs[0x01] = 0;
	set_word(0x04, 3);
	set_word(0x08, 1500);	/* 3300 mV */
	set_word(0x06, (uint16_t)-100 & 0x3fff);
	set_word(0x0b, (uint16_t)-400);
	CHECK(stc3117_task(&g, &r) == 1 && r.soc == 0);

	/* A bus error is an error. */
	fail = true;
	CHECK(stc3117_task(&g, &r) == -1);
	fail = false;

	if (failures)
		printf("%d failures\n", failures);
	else
		printf("stc3117: all passed\n");
	return failures != 0;
}
