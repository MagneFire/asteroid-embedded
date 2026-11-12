/* The Apollo3 over SWD, through memory reads and writes alone. The core
 * is halted, run and read through its debug registers (DHCSR, DCRSR,
 * DCRDR). Flash is written the way the chip's own firmware does it, by
 * the boot ROM's page erase and program helpers: the halted core is
 * pointed at the helper with its arguments in r0-r3 and a return address
 * on a breakpoint in SRAM, and halts there when the helper returns. Only
 * pages from SWD_FLASH_FIRST up are ever touched, and only the two main
 * array helpers are called; the info space and the protection settings,
 * which can lock the debugger out for good, have helpers of their own
 * that this never calls. */
#include <string.h>
#include "swd.h"

#define DHCSR		0xe000edf0u
#define DCRSR		0xe000edf4u
#define DCRDR		0xe000edf8u
#define DEMCR		0xe000edfcu
#define AIRCR		0xe000ed0cu
#define VTOR		0xe000ed08u
#define DBGKEY		0xa05f0000u
#define C_DEBUGEN	(1u << 0)
#define C_HALT		(1u << 1)
#define C_MASKINTS	(1u << 3)
#define S_REGRDY	(1u << 16)
#define S_HALT		(1u << 17)
#define REGWNR		(1u << 16)
#define AIRCR_RESET	0x05fa0004u
#define WDT_CFG		0x40024000u
#define NVIC_ICER0	0xe000e180u
#define NVIC_ICPR0	0xe000e280u

#define REG_SP		13
#define REG_LR		14
#define REG_PC		15
#define REG_XPSR	16
#define REG_MSP		17
#define REG_SPECIAL	20	/* CONTROL, FAULTMASK, BASEPRI, PRIMASK */
#define XPSR_THUMB	0x01000000u

#define ROM_PAGE_ERASE		0x08000051u
#define ROM_PROGRAM_MAIN	0x08000055u
#define PROGRAM_KEY		0x12344321u
#define FLASH_INSTANCE		0x80000u

/* SRAM the helpers' calls use: the breakpoint they return to, the page
 * being written and a stack. */
#define BKPT_ADDR	SWD_SRAM_BASE
#define BKPT_WORD	0xbe00be00u
#define PAGE_BUF	(SWD_SRAM_BASE + 0x1000u)
#define CALL_STACK	(SWD_SRAM_BASE + 0x8000u)
#define CALL_TIMEOUT_US	2000000u

int swd_read(const struct swd_link *l, uint32_t addr, uint32_t *val) { return l->read(l->ctx, addr, val); }
int swd_write(const struct swd_link *l, uint32_t addr, uint32_t val) { return l->write(l->ctx, addr, val); }

int swd_read_block(const struct swd_link *l, uint32_t addr, uint32_t *buf, uint32_t words)
{
	for (uint32_t i = 0; i < words; i++)
		if (swd_read(l, addr + 4 * i, &buf[i]))
			return -1;
	return 0;
}

int swd_write_block(const struct swd_link *l, uint32_t addr, const uint32_t *buf, uint32_t words)
{
	for (uint32_t i = 0; i < words; i++)
		if (swd_write(l, addr + 4 * i, buf[i]))
			return -1;
	return 0;
}

bool swd_halted(const struct swd_link *l)
{
	uint32_t v;
	return !swd_read(l, DHCSR, &v) && (v & S_HALT);
}

static int wait_halt(const struct swd_link *l, uint32_t us)
{
	for (uint32_t t = 0; t < us; t += 100) {
		if (swd_halted(l))
			return 0;
		l->sleep_us(l->ctx, 100);
	}
	return -1;
}

int swd_halt(const struct swd_link *l)
{
	if (swd_write(l, DHCSR, DBGKEY | C_DEBUGEN | C_HALT))
		return -1;
	return wait_halt(l, 100000);
}

int swd_run(const struct swd_link *l) { return swd_write(l, DHCSR, DBGKEY | C_DEBUGEN); }

static int reg_ready(const struct swd_link *l)
{
	for (int i = 0; i < 100; i++) {
		uint32_t v;
		if (swd_read(l, DHCSR, &v))
			return -1;
		if (v & S_REGRDY)
			return 0;
		l->sleep_us(l->ctx, 10);
	}
	return -1;
}

int swd_reg_read(const struct swd_link *l, unsigned reg, uint32_t *val)
{
	if (swd_write(l, DCRSR, reg) || reg_ready(l))
		return -1;
	return swd_read(l, DCRDR, val);
}

int swd_reg_write(const struct swd_link *l, unsigned reg, uint32_t val)
{
	if (swd_write(l, DCRDR, val) || swd_write(l, DCRSR, reg | REGWNR))
		return -1;
	return reg_ready(l);
}

int swd_take(const struct swd_link *l)
{
	if (swd_halt(l))
		return -1;
	/* The watchdog runs on in a halt and would reset the chip midway. */
	if (swd_write(l, WDT_CFG, 0) || swd_write(l, NVIC_ICER0, ~0u) || swd_write(l, NVIC_ICPR0, ~0u))
		return -1;
	return swd_reg_write(l, REG_SPECIAL, 1);
}

int swd_reset(const struct swd_link *l)
{
	uint32_t demcr;
	if (swd_read(l, DEMCR, &demcr) || swd_write(l, DEMCR, demcr & ~1u))
		return -1;
	if (swd_write(l, DHCSR, DBGKEY))
		return -1;
	swd_write(l, AIRCR, AIRCR_RESET);
	return 0;
}

static uint32_t word_at(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

const char *swd_image_check(const uint8_t *img, uint32_t size, uint32_t base)
{
	if (size < 8)
		return "image too small for a vector table";
	uint32_t sp = word_at(img), pc = word_at(img + 4);
	if (sp < SWD_SRAM_BASE || sp > SWD_SRAM_END || (sp & 3))
		return "initial stack pointer outside SRAM";
	if (!(pc & 1))
		return "reset vector is not Thumb code";
	if ((pc & ~1u) < base || (pc & ~1u) >= base + size)
		return "reset vector outside the image";
	return NULL;
}

const char *swd_flash_check(uint32_t addr, uint32_t size)
{
	if (addr % SWD_FLASH_PAGE)
		return "address not at a page";
	if (addr < SWD_FLASH_FIRST)
		return "address inside the boot loader";
	if (!size || size > SWD_FLASH_SIZE - addr || addr >= SWD_FLASH_SIZE)
		return "image beyond the end of flash";
	return NULL;
}

/* Runs a boot ROM helper on the halted core and gives its r0. */
static int rom_call(const struct swd_link *l, uint32_t fn, const uint32_t args[4], uint32_t *ret)
{
	if (fn != ROM_PAGE_ERASE && fn != ROM_PROGRAM_MAIN)
		return -1;
	if (!swd_halted(l) || swd_write(l, BKPT_ADDR, BKPT_WORD))
		return -1;
	for (unsigned r = 0; r < 4; r++)
		if (swd_reg_write(l, r, args[r]))
			return -1;
	if (swd_reg_write(l, REG_MSP, CALL_STACK) || swd_reg_write(l, REG_SP, CALL_STACK) ||
	    swd_reg_write(l, REG_LR, BKPT_ADDR | 1) || swd_reg_write(l, REG_XPSR, XPSR_THUMB) ||
	    swd_reg_write(l, REG_PC, fn & ~1u))
		return -1;
	if (swd_write(l, DHCSR, DBGKEY | C_DEBUGEN | C_MASKINTS) || wait_halt(l, CALL_TIMEOUT_US)) {
		swd_halt(l);
		return -2;
	}
	uint32_t pc;
	if (swd_reg_read(l, REG_PC, &pc) || pc != BKPT_ADDR)
		return -3;
	return swd_reg_read(l, 0, ret);
}

int swd_ram_run(const struct swd_link *l, const uint8_t *img, uint32_t size, uint32_t base)
{
	const char *why = swd_image_check(img, size, base);
	if (why || base < SWD_SRAM_BASE || size > SWD_SRAM_END - base || (base & 0x7f))
		return -1;
	if (swd_take(l))
		return -2;
	uint32_t words = (size + 3) / 4;
	for (uint32_t i = 0; i < words; i++) {
		uint8_t w[4] = { 0 };
		memcpy(w, img + 4 * i, size - 4 * i < 4 ? size - 4 * i : 4);
		if (swd_write(l, base + 4 * i, word_at(w)))
			return -3;
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t v;
		uint8_t w[4] = { 0 };
		memcpy(w, img + 4 * i, size - 4 * i < 4 ? size - 4 * i : 4);
		if (swd_read(l, base + 4 * i, &v) || v != word_at(w))
			return -4;
	}
	uint32_t sp = word_at(img), pc = word_at(img + 4);
	if (swd_write(l, VTOR, base) || swd_reg_write(l, REG_MSP, sp) || swd_reg_write(l, REG_SP, sp) ||
	    swd_reg_write(l, REG_XPSR, XPSR_THUMB) || swd_reg_write(l, REG_PC, pc & ~1u) || swd_reg_write(l, REG_SPECIAL, 0))
		return -5;
	return swd_run(l) ? -6 : 0;
}

int swd_flash(const struct swd_link *l, uint32_t addr, const uint8_t *img, uint32_t size,
	      void (*progress)(uint32_t done, uint32_t total))
{
	if (swd_flash_check(addr, size))
		return -1;
	if (addr == SWD_FLASH_FIRST && swd_image_check(img, size, addr))
		return -1;
	if (swd_take(l))
		return -2;
	static uint32_t page[SWD_FLASH_PAGE / 4], back[SWD_FLASH_PAGE / 4];
	for (uint32_t off = 0; off < size; off += SWD_FLASH_PAGE) {
		uint32_t n = size - off < SWD_FLASH_PAGE ? size - off : SWD_FLASH_PAGE, dst = addr + off, ret = 1;
		memset(page, 0xff, sizeof page);
		memcpy(page, img + off, n);
		if (swd_write_block(l, PAGE_BUF, page, SWD_FLASH_PAGE / 4))
			return -3;
		uint32_t erase[4] = { PROGRAM_KEY, dst / FLASH_INSTANCE, dst % FLASH_INSTANCE / SWD_FLASH_PAGE, 0 };
		if (rom_call(l, ROM_PAGE_ERASE, erase, &ret) || ret)
			return -4;
		uint32_t program[4] = { PROGRAM_KEY, PAGE_BUF, dst, SWD_FLASH_PAGE / 4 };
		if (rom_call(l, ROM_PROGRAM_MAIN, program, &ret) || ret)
			return -5;
		if (swd_read_block(l, dst, back, SWD_FLASH_PAGE / 4) || memcmp(page, back, sizeof page))
			return -6;
		if (progress)
			progress(off + n, size);
	}
	return 0;
}

uint32_t swd_crc32(uint32_t crc, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
	}
	return ~crc;
}
