/* swdtool's SWD logic against a made-up Apollo3: flash, SRAM, the core's
 * debug registers and the boot ROM's two flash helpers, which fail the
 * test if they are ever asked to touch the boot loader, and log the
 * pages they touch in order. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tools/beluga/swd.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define DHCSR 0xe000edf0u
#define DCRSR 0xe000edf4u
#define DCRDR 0xe000edf8u
#define DEMCR 0xe000edfcu
#define MPU_CTRL 0xe000ed94u
#define TRCENA (1u << 24)
#define VC_FAULTS 0x7f0u
#define PARK 0x10000040u
#define PARK_TABLE 0x10000100u

static uint8_t flash[SWD_FLASH_SIZE], sram[SWD_SRAM_END - SWD_SRAM_BASE];
static uint32_t fpccr = 0xc0000001u, icsr_clears, regs[21], dcrdr, wdt_cfg = 0x03000106, vtor, writes_below_first, calls_other, aircr, stray_writes,
	bad_mask_changes;
static bool halted, maskints, wdt_locked;
static int fail_after = -1;
static uint32_t demcr = TRCENA | 1, mpu_ctrl = 5, vendor_runs;
/* Resumes until a core halted in the boot ROM leaves it, 0 for never. */
static unsigned rom_resumes;
/* Which page's program fails, comes out wrong or faults; 0 for none. */
static uint32_t fail_program, corrupt_program, fault_program;

#define LOG_MAX 64
static struct { char op; uint32_t page; } ops[LOG_MAX];
static unsigned nops;

static void log_op(char op, uint32_t page)
{
	if (nops < LOG_MAX)
		ops[nops] = (typeof(ops[0])){ op, page };
	nops++;
}

static bool ops_are(const char *want)
{
	char got[LOG_MAX * 12] = "", *p = got;
	for (unsigned i = 0; i < nops && i < LOG_MAX; i++)
		p += sprintf(p, "%s%c%x", i ? " " : "", ops[i].op, ops[i].page);
	if (strcmp(got, want))
		printf("ops: %s\nwant %s\n", got, want);
	return !strcmp(got, want);
}

static void exec(void)
{
	uint32_t pc = regs[15] | 1, r = 1;
	if (pc == 0x08000051u) {
		uint32_t addr = regs[1] * 0x80000u + regs[2] * SWD_FLASH_PAGE;
		if (regs[0] != 0x12344321u || addr < SWD_FLASH_FIRST)
			writes_below_first++;
		else {
			memset(flash + addr, 0xff, SWD_FLASH_PAGE);
			log_op('E', addr);
			r = 0;
		}
	} else if (pc == 0x08000055u) {
		uint32_t src = regs[1], dst = regs[2], n = regs[3] * 4;
		if (regs[0] != 0x12344321u || dst < SWD_FLASH_FIRST || dst + n > SWD_FLASH_SIZE)
			writes_below_first++;
		else if (dst == fault_program) {
			/* A fault: the vendor's handler unless it is caught. */
			if (!(demcr & (1u << 10)) || !(demcr & VC_FAULTS))
				vendor_runs++;
			regs[15] = 0x0000c0f0u;
			halted = true;
			return;
		} else if (dst != fail_program) {
			for (uint32_t i = 0; i < n; i++)
				flash[dst + i] &= sram[(src - SWD_SRAM_BASE) + i];
			if (dst == corrupt_program)
				flash[dst + 8] = 0;
			log_op('P', dst);
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
		*v = (halted ? 1u << 17 : 0) | 1u << 16 | (maskints ? 8u : 0);
	else if (a == 0x40024000u)
		*v = wdt_cfg;
	else if (a == DCRDR)
		*v = dcrdr;
	else if (a == DEMCR)
		*v = demcr;
	else if (a == 0xe000ef34u)
		*v = fpccr;
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
		if ((bool)(v & 8) != maskints) {
			/* The mask may only change with the core halted and the
			 * halt still asserted in the same write. */
			if (!halted || !(v & 2))
				bad_mask_changes++;
			maskints = v & 8;
		}
		if (v & 2)
			halted = true;
		else if (halted) {
			halted = false;
			if ((regs[15] | 1) >= 0x08000000u && (regs[15] | 1) < 0x08000100u)
				exec();
			else if (regs[15] >= SWD_ROM_BASE && regs[15] < SWD_ROM_END && rom_resumes && !--rom_resumes)
				regs[15] = 0x0000c100u;
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
	else if (a == 0x40024000u) {
		if (!wdt_locked)
			wdt_cfg = v;
	} else if (a == 0xe000ed08u)
		vtor = v;
	else if (a == 0xe000ed0cu)
		aircr = v;
	else if (a == DEMCR)
		demcr = v;
	else if (a == MPU_CTRL)
		mpu_ctrl = v;
	else if (a < SWD_FLASH_SIZE)
		writes_below_first++;	/* flash takes no plain writes */
	else if (a >= SWD_SRAM_BASE && a < SWD_SRAM_END)
		memcpy(sram + (a - SWD_SRAM_BASE), &v, 4);
	else if (a == 0xe000ef34u)
		fpccr = v;
	else if (a == 0xe000ed04u)
		icsr_clears |= v;
	else if (a != 0xe000e180u && a != 0xe000e280u && a != 0xe000e010u)
		stray_writes++;
	return 0;
}

static void nap(void *ctx, uint32_t us) {}

static const struct swd_link link = { .read = rd, .write = wr, .sleep_us = nap };

/* Blocks as the driver does them, word after word underneath. */
static unsigned block_reads, block_writes;
static bool blocks_fail;

static int rd_block(void *ctx, uint32_t a, void *buf, uint32_t n)
{
	block_reads++;
	if (blocks_fail || n % 4)
		return -1;
	for (uint32_t i = 0; i < n; i += 4)
		if (rd(ctx, a + i, (uint32_t *)((uint8_t *)buf + i)))
			return -1;
	return 0;
}

static int wr_block(void *ctx, uint32_t a, const void *buf, uint32_t n)
{
	block_writes++;
	if (blocks_fail || n % 4)
		return -1;
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t v;
		memcpy(&v, (const uint8_t *)buf + i, 4);
		if (wr(ctx, a + i, v))
			return -1;
	}
	return 0;
}

static const struct swd_link block_link = { .read = rd, .write = wr, .sleep_us = nap, .read_block = rd_block,
					    .write_block = wr_block };

static unsigned stop_after;

static bool stopper(uint32_t done, uint32_t total) { return --stop_after > 0; }

static bool erased(uint32_t addr)
{
	for (uint32_t i = 0; i < SWD_FLASH_PAGE; i++)
		if (flash[addr + i] != 0xff)
			return false;
	return true;
}

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

	/* The vector page is erased first and programmed last. */
	nops = 0;
	CHECK(swd_flash(&link, SWD_FLASH_FIRST, img, sizeof img, NULL) == 0);
	CHECK(ops_are("Ec000 Ee000 Pe000 E10000 P10000 Pc000"));
	CHECK(!memcmp(flash + SWD_FLASH_FIRST, img, sizeof img));
	CHECK(flash[SWD_FLASH_FIRST + sizeof img] == 0xff);	/* the rest of the last page */
	CHECK(flash[SWD_FLASH_FIRST - 1] == 0x5a && flash[0] == 0x5a);
	CHECK(flash[SWD_FLASH_FIRST + 0x6000] == 0x5a);
	CHECK(wdt_cfg == 0);
	CHECK(writes_below_first == 0 && calls_other == 0 && stray_writes == 0 && bad_mask_changes == 0);
	/* Faults are caught and the MPU is off; the debugger's other bits stay. */
	CHECK((demcr & VC_FAULTS) == VC_FAULTS && (demcr & TRCENA) && mpu_ctrl == 0);
	/* Let go meanwhile, the core would fault from the breakpoint into a
	 * loop in SRAM, not into flash. */
	CHECK(vtor == PARK_TABLE);
	uint32_t w;
	memcpy(&w, sram + (PARK - SWD_SRAM_BASE), 4);
	CHECK(w == 0xe7fee7feu);
	memcpy(&w, sram + (PARK_TABLE - SWD_SRAM_BASE) + 3 * 4, 4);
	CHECK(w == (PARK | 1));

	/* A failure anywhere after the first erase leaves the vector page
	 * erased and the core halted. */
	static const struct { uint32_t *what, page; int r; const char *ops; } fails[] = {
		{ &fail_program, 0x10000, -5, "Ec000 Ee000 Pe000 E10000" },
		{ &corrupt_program, 0xe000, -6, "Ec000 Ee000 Pe000" },
		{ &fail_program, 0xc000, -5, "Ec000 Ee000 Pe000 E10000 P10000 Ec000" },
		{ &corrupt_program, 0xc000, -6, "Ec000 Ee000 Pe000 E10000 P10000 Pc000 Ec000" },
		{ &fault_program, 0x10000, -5, "Ec000 Ee000 Pe000 E10000" },
	};
	for (unsigned i = 0; i < sizeof fails / sizeof fails[0]; i++) {
		CHECK(swd_flash(&link, SWD_FLASH_FIRST, img, sizeof img, NULL) == 0);
		*fails[i].what = fails[i].page;
		nops = 0;
		CHECK(swd_flash(&link, SWD_FLASH_FIRST, img, sizeof img, NULL) == fails[i].r);
		*fails[i].what = 0;
		CHECK(ops_are(fails[i].ops));
		CHECK(erased(SWD_FLASH_FIRST) && halted);
	}
	CHECK(vendor_runs == 0);

	/* Stopped between pages, the same. */
	stop_after = 1;
	nops = 0;
	CHECK(swd_flash(&link, SWD_FLASH_FIRST, img, sizeof img, stopper) == -7);
	CHECK(ops_are("Ec000 Ee000 Pe000"));
	CHECK(erased(SWD_FLASH_FIRST) && halted);

	/* Pages go through the link's block transfers, and word by word
	 * when those fail. */
	memset(flash + SWD_FLASH_FIRST, 0x5a, 0x6000);
	nops = 0;
	CHECK(swd_flash(&block_link, SWD_FLASH_FIRST, img, sizeof img, NULL) == 0);
	CHECK(ops_are("Ec000 Ee000 Pe000 E10000 P10000 Pc000"));
	CHECK(!memcmp(flash + SWD_FLASH_FIRST, img, sizeof img) && block_writes == 3 && block_reads == 3);
	blocks_fail = true;
	memset(flash + SWD_FLASH_FIRST, 0x5a, 0x6000);
	CHECK(swd_flash(&block_link, SWD_FLASH_FIRST, img, sizeof img, NULL) == 0);
	CHECK(!memcmp(flash + SWD_FLASH_FIRST, img, sizeof img) && block_writes == 6);
	blocks_fail = false;

	/* Halted inside the boot ROM, the core is let run until it is out,
	 * and refused if it stays. */
	halted = maskints = false;
	regs[15] = 0x08000400u;
	rom_resumes = 2;
	CHECK(swd_take(&link) == 0 && halted && regs[15] == 0x0000c100u);
	halted = false;
	regs[15] = 0x08000400u;
	rom_resumes = 0;
	nops = 0;
	CHECK(swd_take(&link) == -4);
	CHECK(swd_flash(&link, SWD_FLASH_FIRST, img, sizeof img, NULL) == -2 && nops == 0);
	CHECK(calls_other == 0 && wdt_cfg == 0);

	/* Parked, the core is set to run the loop with the interrupts off. */
	vtor = 0;
	CHECK(swd_park(&link) == 0);
	CHECK(halted && regs[15] == PARK && regs[20] == 1 && vtor == PARK_TABLE && regs[17] == 0x10008000u);
	CHECK(!(demcr & (VC_FAULTS | 1)) && (demcr & TRCENA));
	CHECK(writes_below_first == 0 && calls_other == 0 && stray_writes == 0);

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
	CHECK(aircr == 0x05fa0002u && fpccr == 0xc0000000u && icsr_clears == 0x0a000000u && regs[20] == 1);
	CHECK(stray_writes == 0);
	CHECK(swd_ram_run(&link, ram, sizeof ram, 0x10020004u) != 0);
	CHECK(swd_ram_run(&link, img, sizeof img, 0x10020000u) != 0);
	CHECK(!maskints && bad_mask_changes == 0);

	/* Anywhere but SRAM is refused before anything is written. */
	static const uint32_t not_sram[] = { 0x40020000u, 0x50020000u, 0xffffff80u, 0x0000c000u, 0x10000000u,
					     SWD_SRAM_END - 0x80u, SWD_SRAM_END };
	for (unsigned i = 0; i < sizeof not_sram / sizeof not_sram[0]; i++) {
		static uint8_t evil[0x400];
		image(evil, sizeof evil, not_sram[i], 0x10010000u);
		uint32_t before = stray_writes;
		CHECK(swd_ram_check(not_sram[i], sizeof evil) != NULL);
		CHECK(swd_ram_run(&link, evil, sizeof evil, not_sram[i]) == -1);
		CHECK(stray_writes == before);
	}

	/* A watchdog that stays on stops a flash before any erase. */
	wdt_locked = true;
	wdt_cfg = 0x03000107u;
	memset(flash + SWD_FLASH_FIRST, 0x11, SWD_FLASH_PAGE);
	CHECK(swd_flash(&link, SWD_FLASH_FIRST, img, sizeof img, NULL) == -2);
	CHECK(flash[SWD_FLASH_FIRST] == 0x11);
	wdt_locked = false;

	halted = true;
	maskints = true;
	demcr = TRCENA | VC_FAULTS | 1;
	CHECK(swd_reset(&link) == 0 && aircr == 0x05fa0004u);
	CHECK(demcr == TRCENA);
	CHECK(!maskints);
	halted = false;
	maskints = true;
	uint32_t changes = bad_mask_changes;
	CHECK(swd_unmask(&link) == 0 && !maskints && !halted && bad_mask_changes == changes);
	CHECK(swd_unmask(&link) == 0 && !halted);
	fail_after = 1;
	CHECK(swd_reset(&link) != 0);
	fail_after = -1;
	CHECK(swd_crc32(0, "123456789", 9) == 0xcbf43926u);

	printf("%s\n", failures ? "swd_test: FAILED" : "swd_test: ok");
	return failures ? 1 : 0;
}
