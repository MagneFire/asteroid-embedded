/* swdtool's SWD logic against a made-up Apollo3: flash, SRAM, the core's
 * debug registers and the boot ROM's two flash helpers, which fail the
 * test if they are ever asked to touch the boot loader. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tools/beluga/swd.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define DHCSR 0xe000edf0u
#define DCRSR 0xe000edf4u
#define DCRDR 0xe000edf8u

static uint8_t flash[SWD_FLASH_SIZE], sram[SWD_SRAM_END - SWD_SRAM_BASE];
static uint32_t regs[21], dcrdr, wdt_cfg = 0x03000106, vtor, writes_below_first, calls_other, aircr;
static bool halted, maskints;
static int fail_after = -1;

static void exec(void)
{
	uint32_t pc = regs[15] | 1, r = 1;
	if (pc == 0x08000051u) {
		uint32_t addr = regs[1] * 0x80000u + regs[2] * SWD_FLASH_PAGE;
		if (regs[0] != 0x12344321u || addr < SWD_FLASH_FIRST)
			writes_below_first++;
		else {
			memset(flash + addr, 0xff, SWD_FLASH_PAGE);
			r = 0;
		}
	} else if (pc == 0x08000055u) {
		uint32_t src = regs[1], dst = regs[2], n = regs[3] * 4;
		if (regs[0] != 0x12344321u || dst < SWD_FLASH_FIRST || dst + n > SWD_FLASH_SIZE)
			writes_below_first++;
		else {
			for (uint32_t i = 0; i < n; i++)
				flash[dst + i] &= sram[(src - SWD_SRAM_BASE) + i];
			r = 0;
		}
	} else
		calls_other++;
	regs[0] = r;
	regs[15] = regs[14] & ~1u;
	halted = true;
}

static int rd(void *ctx, uint32_t a, uint32_t *v)
{
	if (fail_after == 0)
		return -1;
	if (a == DHCSR)
		*v = (halted ? 1u << 17 : 0) | 1u << 16;
	else if (a == DCRDR)
		*v = dcrdr;
	else if (a < SWD_FLASH_SIZE)
		memcpy(v, flash + a, 4);
	else if (a >= SWD_SRAM_BASE && a < SWD_SRAM_END)
		memcpy(v, sram + (a - SWD_SRAM_BASE), 4);
	else
		*v = 0;
	return 0;
}

static int wr(void *ctx, uint32_t a, uint32_t v)
{
	if (fail_after > 0)
		fail_after--;
	if (fail_after == 0)
		return -1;
	if (a == DHCSR) {
		if ((v >> 16) != 0xa05f)
			return 0;
		if (v & 2)
			halted = true;
		else if (halted) {
			maskints = v & 8;
			halted = false;
			if ((regs[15] | 1) >= 0x08000000u && (regs[15] | 1) < 0x08000100u)
				exec();
		}
	} else if (a == DCRSR) {
		unsigned r = v & 0x7f;
		if (r < 21) {
			if (v & 0x10000)
				regs[r] = dcrdr;
			else
				dcrdr = regs[r];
		}
	} else if (a == DCRDR)
		dcrdr = v;
	else if (a == 0x40024000u)
		wdt_cfg = v;
	else if (a == 0xe000ed08u)
		vtor = v;
	else if (a == 0xe000ed0cu)
		aircr = v;
	else if (a < SWD_FLASH_SIZE)
		writes_below_first++;	/* flash takes no plain writes */
	else if (a >= SWD_SRAM_BASE && a < SWD_SRAM_END)
		memcpy(sram + (a - SWD_SRAM_BASE), &v, 4);
	return 0;
}

static void nap(void *ctx, uint32_t us) {}

static const struct swd_link link = { rd, wr, nap, NULL };

static void image(uint8_t *p, uint32_t size, uint32_t base, uint32_t sp)
{
	for (uint32_t i = 0; i < size; i++)
		p[i] = (uint8_t)(i * 7 + 3);
	uint32_t pc = base + 0x101;
	memcpy(p, &sp, 4);
	memcpy(p + 4, &pc, 4);
}

int main(void)
{
	memset(flash, 0x5a, sizeof flash);
	static uint8_t img[0x5100];
	image(img, sizeof img, SWD_FLASH_FIRST, 0x10010000u);

	CHECK(swd_flash_check(0, 0x2000) != NULL);
	CHECK(swd_flash_check(0xa000, 0x4000) != NULL);
	CHECK(swd_flash_check(0xc001, 0x100) != NULL);
	CHECK(swd_flash_check(0xc000, 0x100001) != NULL);
	CHECK(swd_flash_check(0xfe000, 0x2001) != NULL);
	CHECK(swd_flash_check(0xc000, 0xf4000) == NULL);
	CHECK(swd_image_check(img, sizeof img, SWD_FLASH_FIRST) == NULL);

	uint8_t bad[16] = { 0 };
	CHECK(swd_image_check(bad, sizeof bad, SWD_FLASH_FIRST) != NULL);
	CHECK(swd_flash(&link, SWD_FLASH_FIRST, bad, sizeof bad, NULL) == -1);
	CHECK(swd_flash(&link, 0x8000, img, sizeof img, NULL) == -1);

	CHECK(swd_flash(&link, SWD_FLASH_FIRST, img, sizeof img, NULL) == 0);
	CHECK(!memcmp(flash + SWD_FLASH_FIRST, img, sizeof img));
	CHECK(flash[SWD_FLASH_FIRST + sizeof img] == 0xff);	/* the rest of the last page */
	CHECK(flash[SWD_FLASH_FIRST - 1] == 0x5a && flash[0] == 0x5a);
	CHECK(flash[SWD_FLASH_FIRST + 0x6000] == 0x5a);
	CHECK(wdt_cfg == 0);
	CHECK(writes_below_first == 0 && calls_other == 0);
	CHECK(maskints);

	/* A link that dies halfway leaves an error, never a write lower. */
	fail_after = 2000;
	CHECK(swd_flash(&link, SWD_FLASH_FIRST, img, sizeof img, NULL) < 0);
	fail_after = -1;
	CHECK(writes_below_first == 0);

	static uint8_t ram[0x400];
	image(ram, sizeof ram, 0x10020000u, 0x10030000u);
	CHECK(swd_ram_run(&link, ram, sizeof ram, 0x10020000u) == 0);
	CHECK(!memcmp(sram + 0x20000, ram, sizeof ram));
	CHECK(vtor == 0x10020000u && regs[17] == 0x10030000u && regs[15] == 0x10020100u && !halted);
	CHECK(swd_ram_run(&link, ram, sizeof ram, 0x10020004u) != 0);
	CHECK(swd_ram_run(&link, img, sizeof img, 0x10020000u) != 0);

	CHECK(swd_reset(&link) == 0 && aircr == 0x05fa0004u);
	CHECK(swd_crc32(0, "123456789", 9) == 0xcbf43926u);

	printf("%s\n", failures ? "swd_test: FAILED" : "swd_test: ok");
	return failures ? 1 : 0;
}
