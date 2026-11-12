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
#define MPU_CTRL	0xe000ed94u
#define ICSR		0xe000ed04u
#define ICSR_PENDCLR	((1u << 27) | (1u << 25))	/* PendSV, SysTick */
#define SYST_CSR	0xe000e010u
#define FPCCR		0xe000ef34u
#define FPCCR_LSPACT	(1u << 0)
#define AIRCR_CLRACTIVE	0x05fa0002u
#define DBGKEY		0xa05f0000u
#define C_DEBUGEN	(1u << 0)
#define C_HALT		(1u << 1)
#define C_MASKINTS	(1u << 3)
#define S_REGRDY	(1u << 16)
#define S_HALT		(1u << 17)
#define REGWNR		(1u << 16)
#define AIRCR_RESET	0x05fa0004u
#define VC_CORERESET	(1u << 0)
#define VC_FAULTS	0x7f0u	/* MMERR, NOCPERR, CHKERR, STATERR, BUSERR, INTERR, HARDERR */
#define WDT_CFG		0x40024000u
#define WDT_ENABLED	1u
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
 * being written and a stack; and a loop with a vector table of its own
 * for a core left parked. */
#define BKPT_ADDR	SWD_SRAM_BASE
#define BKPT_WORD	0xbe00be00u
#define PARK_ADDR	(SWD_SRAM_BASE + 0x40u)
#define PARK_WORD	0xe7fee7feu
#define PARK_TABLE	(SWD_SRAM_BASE + 0x100u)
#define PARK_VECTORS	48u
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

/* C_MASKINTS may only change while the core is halted: set it with the
 * halt still asserted, then release the halt with it unchanged. */
static int release(const struct swd_link *l, bool mask)
{
	uint32_t m = mask ? C_MASKINTS : 0;
	if (swd_write(l, DHCSR, DBGKEY | C_DEBUGEN | C_HALT | m))
		return -1;
	return swd_write(l, DHCSR, DBGKEY | C_DEBUGEN | m);
}

int swd_run(const struct swd_link *l) { return release(l, false); }

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

static int vector_catch(const struct swd_link *l, uint32_t set, uint32_t clear)
{
	uint32_t demcr;
	if (swd_read(l, DEMCR, &demcr))
		return -1;
	return swd_write(l, DEMCR, (demcr & ~clear) | set);
}

int swd_take(const struct swd_link *l)
{
	if (swd_halt(l))
		return -1;
	/* The watchdog runs on in a halt and would reset the chip midway.
	 * A locked one takes no write, so it is read back. */
	uint32_t wdt;
	if (swd_write(l, WDT_CFG, 0) || swd_read(l, WDT_CFG, &wdt))
		return -1;
	if (wdt & WDT_ENABLED)
		return -3;
	if (swd_write(l, NVIC_ICER0, ~0u) || swd_write(l, NVIC_ICPR0, ~0u))
		return -1;
	/* Faults are not masked, and the vendor's handlers would take them:
	 * they halt the core instead. The vendor's MPU could keep the
	 * breakpoint in SRAM from running. */
	if (vector_catch(l, VC_FAULTS, 0) || swd_write(l, MPU_CTRL, 0))
		return -1;
	return swd_reg_write(l, REG_SPECIAL, 1);
}

/* The reset is requested with the core still halted, so it never runs
 * on from where the tool left it; debug stays enabled until the chip is
 * back from the boot loader. */
int swd_reset(const struct swd_link *l)
{
	if (vector_catch(l, 0, VC_CORERESET | VC_FAULTS))
		return -1;
	return swd_write(l, AIRCR, AIRCR_RESET) ? -1 : 0;
}

/* The loop and its table are written before the flash starts, so that
 * a core let go without the tool (the device closed, the tool killed)
 * faults into the loop from the breakpoint and not into flash. */
static int park_setup(const struct swd_link *l)
{
	if (swd_write(l, PARK_ADDR, PARK_WORD))
		return -1;
	if (swd_write(l, PARK_TABLE, CALL_STACK))
		return -1;
	for (uint32_t i = 1; i < PARK_VECTORS; i++)
		if (swd_write(l, PARK_TABLE + 4 * i, PARK_ADDR | 1))
			return -1;
	return swd_write(l, VTOR, PARK_TABLE);
}

int swd_park(const struct swd_link *l)
{
	if ((!swd_halted(l) && swd_halt(l)) || park_setup(l))
		return -1;
	if (swd_reg_write(l, REG_MSP, CALL_STACK) || swd_reg_write(l, REG_SP, CALL_STACK) ||
	    swd_reg_write(l, REG_XPSR, XPSR_THUMB) || swd_reg_write(l, REG_PC, PARK_ADDR) ||
	    swd_reg_write(l, REG_SPECIAL, 1))
		return -1;
	return vector_catch(l, 0, VC_CORERESET | VC_FAULTS);
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
	if (sp <= SWD_SRAM_BASE || sp > SWD_SRAM_END || (sp & 7))
		return "initial stack pointer outside SRAM";
	if (!(pc & 1))
		return "reset vector is not Thumb code";
	if ((pc & ~1u) < base || (pc & ~1u) - base >= size)
		return "reset vector outside the image";
	return NULL;
}

const char *swd_ram_check(uint32_t base, uint32_t size)
{
	if (base < SWD_SRAM_BASE || base >= SWD_SRAM_END)
		return "load address outside SRAM";
	if (base & 0x7f)
		return "load address not 128-byte aligned";
	if (!size || size > SWD_SRAM_END - base)
		return "image does not fit in SRAM there";
	if (base < CALL_STACK)
		return "load address over the tool's own scratch SRAM";
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
	if (release(l, true) || wait_halt(l, CALL_TIMEOUT_US)) {
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
	if (swd_ram_check(base, size) || swd_image_check(img, size, base))
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
	/* The vendor's core was stopped anywhere, maybe in a handler: its
	 * active and pending exceptions, its tick and its lazy FPU frame are
	 * dropped, and the image starts with interrupts masked. */
	uint32_t fpccr;
	if (swd_write(l, AIRCR, AIRCR_CLRACTIVE) || swd_write(l, SYST_CSR, 0) || swd_write(l, ICSR, ICSR_PENDCLR) ||
	    swd_write(l, NVIC_ICPR0, ~0u) || swd_read(l, FPCCR, &fpccr) || swd_write(l, FPCCR, fpccr & ~FPCCR_LSPACT))
		return -5;
	uint32_t sp = word_at(img), pc = word_at(img + 4);
	if (vector_catch(l, 0, VC_FAULTS) || swd_write(l, VTOR, base) || swd_reg_write(l, REG_MSP, sp) || swd_reg_write(l, REG_SP, sp) ||
	    swd_reg_write(l, REG_XPSR, XPSR_THUMB) || swd_reg_write(l, REG_PC, pc & ~1u) || swd_reg_write(l, REG_SPECIAL, 1))
		return -5;
	return release(l, false) ? -6 : 0;
}

static int erase(const struct swd_link *l, uint32_t dst)
{
	if (swd_flash_check(dst, SWD_FLASH_PAGE))
		return -4;
	uint32_t args[4] = { PROGRAM_KEY, dst / FLASH_INSTANCE, dst % FLASH_INSTANCE / SWD_FLASH_PAGE, 0 }, ret = 1;
	return rom_call(l, ROM_PAGE_ERASE, args, &ret) || ret ? -4 : 0;
}

/* Programs an erased page from the image and reads it back. */
static int program(const struct swd_link *l, uint32_t dst, const uint8_t *src, uint32_t n)
{
	static uint32_t page[SWD_FLASH_PAGE / 4], back[SWD_FLASH_PAGE / 4];
	if (swd_flash_check(dst, SWD_FLASH_PAGE) || n > SWD_FLASH_PAGE)
		return -5;
	memset(page, 0xff, sizeof page);
	memcpy(page, src, n);
	if (swd_write_block(l, PAGE_BUF, page, SWD_FLASH_PAGE / 4))
		return -3;
	uint32_t args[4] = { PROGRAM_KEY, PAGE_BUF, dst, SWD_FLASH_PAGE / 4 }, ret = 1;
	if (rom_call(l, ROM_PROGRAM_MAIN, args, &ret) || ret)
		return -5;
	if (swd_read_block(l, dst, back, SWD_FLASH_PAGE / 4) || memcmp(page, back, sizeof page))
		return -6;
	return 0;
}

/* The first page, with the vector table, is erased first and programmed
 * last, so that a flash that stops anywhere leaves no vectors into a mix
 * of two images; one that fails programming it erases it again. */
int swd_flash(const struct swd_link *l, uint32_t addr, const uint8_t *img, uint32_t size,
	      bool (*progress)(uint32_t done, uint32_t total))
{
	if (swd_flash_check(addr, size))
		return -1;
	if (addr == SWD_FLASH_FIRST && swd_image_check(img, size, addr))
		return -1;
	if (swd_take(l))
		return -2;
	if (park_setup(l))
		return -3;
	int r = erase(l, addr);
	if (r)
		return r;
	uint32_t first = size < SWD_FLASH_PAGE ? size : SWD_FLASH_PAGE;
	for (uint32_t off = SWD_FLASH_PAGE; off < size; off += SWD_FLASH_PAGE) {
		uint32_t n = size - off < SWD_FLASH_PAGE ? size - off : SWD_FLASH_PAGE;
		if ((r = erase(l, addr + off)) || (r = program(l, addr + off, img + off, n)))
			return r;
		if (progress && !progress(off, size))
			return -7;
	}
	if ((r = program(l, addr, img, first))) {
		erase(l, addr);
		return r;
	}
	if (progress)
		progress(size, size);
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
