/* PowerVR SGX530: power, reset, its page tables and the microkernel
 * that runs on it. The GPU does nothing by register: a program on its
 * own processor takes commands from a queue in memory, and that
 * program comes from the watch's system partition (sgx_image.c). The
 * host's part is to map memory, start the program, queue commands and
 * kick. */
#include <string.h>
#include "asteroid.h"
#include "sgx.h"
#include "sgx_image.h"

#define LIBRARY			"/lib/libsrv_init.so"
#define SGX_HZ			192000000u

#define REG(off)		(SGX_BASE + (off))
#define CLKGATECTL		REG(0x000)
#define CLKGATESTATUS		REG(0x004)
#define CORE_ID			REG(0x010)
#define CORE_REVISION		REG(0x014)
#define SOFT_RESET		REG(0x080)
#define SOFT_RESET_BIF		BIT(0)
#define SOFT_RESET_ALL		0x7fu
#define EVENT_HOST_CLEAR2	REG(0x114)
#define EVENT_STATUS2		REG(0x118)
#define EVENT_STATUS		REG(0x12c)
#define EVENT_HOST_ENABLE	REG(0x130)
#define EVENT_HOST_CLEAR	REG(0x134)
#define EVENT_SW		BIT(14)
#define EVENT_MASTER		BIT(31)
#define EVENT_KICK		REG(0xac8)
#define BIF_CTRL		REG(0xc00)
#define BIF_CTRL_INVALDC	BIT(3)
#define BIF_INT_STAT		REG(0xc04)
#define BIF_FAULT_MASK		0x3fffu
#define BIF_FAULT		REG(0xc08)
#define BIF_DIR_LIST_BASE0	REG(0xc84)
#define BIF_MEM_REQ_STAT	REG(0xca8)
#define OCP_REVISION		REG(0xfe00)
#define OCP_SYSCONFIG		REG(0xfe10)
#define OCP_SMART_IDLE_STANDBY	0x28u
#define OCP_IRQSTATUS_2		REG(0xfe38)
#define OCP_IRQENABLE_SET_2	REG(0xfe44)
#define OCP_IRQENABLE_CLR_2	REG(0xfe50)

#define SGX_FCLK		BIT(1)
#define SGX_ICLK		BIT(0)
#define CLKSEL_192M		4u
#define CLKGATE_AUTO		0x00222220u

/* The words of the block the host and the microkernel share. */
enum {
	HOST_INIT_STATUS,
	HOST_POWER_STATUS,
	HOST_CLEANUP_STATUS,
	HOST_INVAL_STATUS,
	HOST_UKERNEL_LOCKUPS,
	HOST_HOST_LOCKUPS,
	HOST_RECOVERY_RATE,
	HOST_TIMER_CLOCK,
	HOST_ACTIVE_POWER_RATE,
	HOST_INTERRUPT_FLAGS,
	HOST_INTERRUPT_CLEAR_FLAGS,
	HOST_BREAKPOINT,
	HOST_ACTIVE_POWER_EVENTS,
	HOST_TIME_WRAPS,
	HOST_CLOCK,
	HOST_ASSERT_FAIL,
	HOST_WORDS = 19
};
#define INIT_COMPLETE		BIT(0)
#define POWER_OFF_COMPLETE	BIT(3)
#define INTERRUPT_ACTIVE_POWER	BIT(1)
#define INTERRUPT_IDLE		BIT(2)

#define POWER_OFF		1u
#define MISC_READY		1u
#define MISC_STRUCT_SIZES	2u
#define BUILD_OPTIONS		0x0016801cu

#define HW_WAIT_US		1000000u

static struct sgx_mem mem;
static struct sgx_image img;
static bool built, running, reset_once;
static volatile uint32_t *host, *ccb, *ccb_ctl, *kicker;
static uint32_t reset_pd, reset_pt, reset_page;
static uint32_t cache_control, core_id, core_revision, start_us;
static volatile uint32_t events, stray;

static bool wait_reg(uint32_t addr, uint32_t mask, uint32_t want, uint32_t us)
{
	uint32_t t0 = time_us();
	while ((readl(addr) & mask) != want)
		if (time_us() - t0 > us)
			return false;
	return true;
}

static bool wait_word(volatile uint32_t *word, uint32_t mask, uint32_t us)
{
	uint32_t t0 = time_us();
	while (!(*word & mask)) {
		if (time_us() - t0 > us)
			return false;
		wdt_feed();
	}
	return true;
}

static void power_off(void)
{
	writel(0, CM_SGX + CM_FCLKEN);
	writel(0, CM_SGX + CM_ICLKEN);
	writel(readl(PRM_SGX + PM_PWSTCTRL) & ~3u, PRM_SGX + PM_PWSTCTRL);
	writel(3, CM_SGX + CM_CLKSTCTRL);
}

static bool power_on(void)
{
	writel((readl(CM_SGX + CM_CLKSEL) & ~7u) | CLKSEL_192M, CM_SGX + CM_CLKSEL);
	writel(readl(PRM_SGX + PM_PWSTCTRL) | 3, PRM_SGX + PM_PWSTCTRL);
	/* Held awake while in use, as the display's domain is: left to
	 * itself the domain may stop the clock under a register read. */
	writel(2, CM_SGX + CM_CLKSTCTRL);
	writel(SGX_FCLK, CM_SGX + CM_FCLKEN);
	writel(SGX_ICLK, CM_SGX + CM_ICLKEN);
	/* IDLEST says whether the GPU is in standby, which it is for as
	 * long as it has nothing to do, so only the domain is waited for. */
	if (!wait_reg(PRM_SGX + PM_PWSTST, 3, 3, 100000)) {
		log_printf("sgx: no power, pwstst %08lx\n", readl(PRM_SGX + PM_PWSTST));
		return false;
	}
	writel(OCP_SMART_IDLE_STANDBY, OCP_SYSCONFIG);
	return true;
}

static bool read_library(void *ctx, uint32_t off, void *buf, uint32_t len)
{
	return firmware_read(LIBRARY, off, buf, len) == (int)len;
}

static bool build(void)
{
	static uint8_t chunk[4096] __attribute__((aligned(64)));
	uint32_t crc = 0, t0 = time_us();
	for (uint32_t off = SGX_FILE_FIRST; off < SGX_FILE_END; off += sizeof chunk) {
		uint32_t n = SGX_FILE_END - off < sizeof chunk ? SGX_FILE_END - off : sizeof chunk;
		if (!read_library(NULL, off, chunk, n)) {
			log_printf("sgx: no %s on the system partition\n", LIBRARY);
			return false;
		}
		crc = sgx_crc32(crc, chunk, n);
		wdt_feed();
	}
	if (crc != SGX_FILE_CRC) {
		log_printf("sgx: %s is another build, crc %08lx\n", LIBRARY, crc);
		return false;
	}
	sgx_mem_init(&mem, (void *)SGX_MEM_BASE, SGX_MEM_BASE, SGX_MEM_SIZE);
	if (!sgx_image_build(&mem, &img, read_library, NULL) || !(reset_pd = sgx_pages(&mem, 3))) {
		log_printf("sgx: image not built\n");
		return false;
	}
	reset_pt = reset_pd + SGX_PAGE;
	reset_page = reset_pt + SGX_PAGE;
	host = sgx_ptr(&mem, img.host_ctl);
	ccb = sgx_ptr(&mem, img.ccb);
	ccb_ctl = sgx_ptr(&mem, img.ccb_ctl);
	kicker = sgx_ptr(&mem, img.kicker);
	cache_control = SGX_CACHE_INVAL_PD;
	log_printf("sgx: image of %lu KiB built in %lu us\n", mem.used >> 10, time_us() - t0);
	return true;
}

static void run_script(unsigned which)
{
	struct sgx_reg r[SGX_SCRIPT_MAX];
	unsigned n = sgx_script(&img, which, r);
	for (unsigned i = 0; i < n; i++)
		writel(r[i].value, REG(r[i].off));
}

/* A hundred of the GPU's clocks, which each reset step is given. */
static void settle(void) { delay_us(2); }

static void invalidate_directory_cache(void)
{
	writel(BIF_CTRL_INVALDC, BIF_CTRL);
	settle();
	writel(0, BIF_CTRL);
	settle();
	if (!wait_reg(BIF_MEM_REQ_STAT, 0xff, 0, HW_WAIT_US))
		log_printf("sgx: memory requests still out, %08lx\n", readl(BIF_MEM_REQ_STAT));
}

static void reset(void)
{
	writel(SOFT_RESET_ALL, SOFT_RESET);
	settle();
	writel(0, BIF_CTRL);
	writel(0, BIF_DIR_LIST_BASE0);
	if (!reset_once) {
		/* Parts of the GPU have no reset of their own, and what a
		 * system that ran before left them asking for is let out
		 * first: against a directory that maps nothing every request
		 * faults, and each fault is given a page until none is left. */
		uint32_t *pd = (uint32_t *)reset_pd, *pt = (uint32_t *)reset_pt;
		writel(reset_pd, BIF_DIR_LIST_BASE0);
		writel(SOFT_RESET_ALL & ~SOFT_RESET_BIF, SOFT_RESET);
		settle();
		invalidate_directory_cache();
		for (unsigned i = 0; i < 64 && (readl(BIF_INT_STAT) & BIF_FAULT_MASK); i++) {
			uint32_t fault = readl(BIF_FAULT);
			log_printf("sgx: reset let out a request for %08lx\n", fault);
			writel(SOFT_RESET_ALL, SOFT_RESET);
			pd[fault >> 22] = reset_pt | 1;
			pt[fault >> 12 & 0x3ff] = reset_page | 1;
			writel(readl(EVENT_STATUS), EVENT_HOST_CLEAR);
			writel(readl(EVENT_STATUS2), EVENT_HOST_CLEAR2);
			settle();
			writel(SOFT_RESET_ALL & ~SOFT_RESET_BIF, SOFT_RESET);
			settle();
			invalidate_directory_cache();
			pd[fault >> 22] = 0;
			pt[fault >> 12 & 0x3ff] = 0;
		}
		reset_once = true;
	} else {
		writel(SOFT_RESET_ALL & ~SOFT_RESET_BIF, SOFT_RESET);
		settle();
	}
	writel(mem.phys, BIF_DIR_LIST_BASE0);
	invalidate_directory_cache();
	writel(0, SOFT_RESET);
	settle();
}

static void sgx_irq(void)
{
	uint32_t status = readl(EVENT_STATUS) & readl(EVENT_HOST_ENABLE);
	if (status & EVENT_SW) {
		writel(EVENT_SW | EVENT_MASTER, EVENT_HOST_CLEAR);
		writel(0, EVENT_HOST_CLEAR2);
		events++;
	} else
		stray++;
	writel(1, OCP_IRQSTATUS_2);
	readl(EVENT_HOST_CLEAR);
	/* The microkernel tells when it has run out of work and waits to
	 * hear that this was seen. */
	if (host[HOST_INTERRUPT_FLAGS] & ~host[HOST_INTERRUPT_CLEAR_FLAGS] & INTERRUPT_IDLE)
		host[HOST_INTERRUPT_CLEAR_FLAGS] |= INTERRUPT_IDLE;
}

static void kick(void)
{
	*kicker = (*kicker + 1) & 0xff;
	dsb();
	writel(1, EVENT_KICK);
}

static bool command(unsigned type, uint32_t arg, uint32_t cache)
{
	uint32_t t0 = time_us(), at, words[8];
	while ((((at = ccb_ctl[0]) + 1) & 0xff) == ccb_ctl[1])
		if (time_us() - t0 > HW_WAIT_US) {
			log_printf("sgx: command queue full\n");
			return false;
		}
	sgx_command_words(words, type, cache_control | cache, arg);
	for (unsigned i = 0; i < 8; i++)
		ccb[at * 8 + i] = words[i];
	cache_control = 0;
	ccb_ctl[0] = (at + 1) & 0xff;
	uint32_t flags = irq_save();
	host[HOST_INTERRUPT_CLEAR_FLAGS] |= INTERRUPT_IDLE | INTERRUPT_ACTIVE_POWER;
	irq_restore(flags);
	kick();
	return true;
}

void sgx_log_state(void)
{
	static const uint16_t regs[] = { 0x000, 0x004, 0x118, 0x12c, 0x4e0, 0x4e4, 0x658, 0xa74, 0xc04, 0xc08, 0x624, 0x628,
					 0x630, 0x734, 0xaa4, 0xaa8, 0xb08, 0xb14, 0xb0c, 0xb18, 0xb10, 0xb1c };
	if (!(readl(PRM_SGX + PM_PWSTST) & 3) || !(readl(CM_SGX + CM_FCLKEN) & SGX_FCLK)) {
		log_printf("sgx: off\n");
		return;
	}
	for (unsigned i = 0; i < ARRAY_SIZE(regs); i++)
		log_printf("sgx: reg %03x = %08lx\n", regs[i], readl(REG(regs[i])));
	if (!built)
		return;
	for (unsigned i = 0; i < HOST_WORDS; i++)
		log_printf("sgx: host %u = %08lx\n", i, host[i]);
	log_printf("sgx: queue write %lu read %lu, kicker %lu, events %lu, stray %lu\n", ccb_ctl[0], ccb_ctl[1], *kicker,
		   events, stray);
	/* The trace is a ring of four words a step behind two words that
	 * say where it is. */
	const uint32_t *trace = sgx_ptr(&mem, img.trace);
	log_printf("sgx: trace %08lx %08lx\n", trace[0], trace[1]);
	for (unsigned i = 0; i < 512; i++) {
		const uint32_t *e = trace + 2 + i * 4;
		if (e[0] | e[1] | e[2] | e[3])
			log_printf("sgx: trace %u: %08lx %08lx %08lx %08lx\n", i, e[0], e[1], e[2], e[3]);
		wdt_feed();
	}
}

bool sgx_running(void) { return running; }

struct sgx_mem *sgx_memory(void) { return built ? &mem : NULL; }

bool sgx_submit_ta(uint32_t context, uint32_t cache)
{
	return running && command(SGX_CMD_TA, context, cache);
}

bool sgx_start(void)
{
	if (running)
		return true;
	uint32_t t0 = time_us();
	if (!power_on()) {
		power_off();
		return false;
	}
	core_id = readl(CORE_ID);
	core_revision = readl(CORE_REVISION);
	if (!built && !(built = build())) {
		power_off();
		return false;
	}
	host[HOST_POWER_STATUS] = 0;
	host[HOST_RECOVERY_RATE] = 10;
	host[HOST_TIMER_CLOCK] = SGX_HZ / 1000;
	/* The microkernel is not asked to propose power-downs: the GPU
	 * goes off when the watch sleeps. */
	host[HOST_ACTIVE_POWER_RATE] = 0;
	writel(CLKGATE_AUTO, CLKGATECTL);
	run_script(SGX_SCRIPT_BEFORE_RESET);
	reset();
	*kicker = 0;
	run_script(SGX_SCRIPT_AFTER_RESET);
	host[HOST_CLOCK] = time_us();
	host[HOST_INIT_STATUS] = 0;
	kick();
	if (!wait_word(&host[HOST_INIT_STATUS], INIT_COMPLETE, HW_WAIT_US)) {
		log_printf("sgx: microkernel did not start, core %08lx revision %08lx\n", core_id, core_revision);
		sgx_log_state();
		power_off();
		return false;
	}
	writel(1, OCP_IRQSTATUS_2);
	writel(1, OCP_IRQENABLE_SET_2);
	irq_attach(IRQ_SGX, sgx_irq);
	running = true;
	start_us = time_us() - t0;
	log_printf("sgx: core %08lx revision %08lx, microkernel up in %lu us\n", core_id, core_revision, start_us);
	return true;
}

void sgx_stop(void)
{
	if (!running)
		return;
	running = false;
	host[HOST_POWER_STATUS] = 0;
	if (!command(SGX_CMD_POWER, POWER_OFF, 0) || !wait_word(&host[HOST_POWER_STATUS], POWER_OFF_COMPLETE, HW_WAIT_US) ||
	    !wait_reg(CLKGATESTATUS, 0xffffffff, 0, HW_WAIT_US)) {
		log_printf("sgx: microkernel did not stop\n");
		sgx_log_state();
	}
	irq_mask(IRQ_SGX);
	writel(1, OCP_IRQENABLE_CLR_2);
	run_script(SGX_SCRIPT_OFF);
	power_off();
}

void sgx_halt(void)
{
	if (!running)
		return;
	running = false;
	irq_mask(IRQ_SGX);
	writel(1, OCP_IRQENABLE_CLR_2);
	writel(SOFT_RESET_ALL, SOFT_RESET);
	settle();
	run_script(SGX_SCRIPT_OFF);
	power_off();
}

bool sgx_query(struct sgx_info *info)
{
	static const uint32_t sizes[13] = { 1304, 1128, 1204, 268, 136, 68, 64, 1040, 52, 76, 228, 76, 32 };
	if (!running)
		return false;
	volatile uint32_t *misc = sgx_ptr(&mem, img.misc);
	for (unsigned i = 1; i < 21; i++)
		misc[i] = 0;
	misc[0] = MISC_STRUCT_SIZES;
	if (!command(SGX_CMD_GETMISCINFO, img.misc, 0) || !wait_word(&misc[0], MISC_READY, HW_WAIT_US)) {
		log_printf("sgx: no answer to the query\n");
		sgx_log_state();
		return false;
	}
	info->core_id = core_id;
	info->core_revision = core_revision;
	info->ddk_version = misc[3];
	info->ddk_build = misc[4];
	info->software_core = misc[5];
	info->software_revision = misc[6];
	info->build_options = misc[7];
	info->start_us = start_us;
	info->structs_match = misc[7] == BUILD_OPTIONS;
	for (unsigned i = 0; i < 13; i++)
		if (misc[8 + i] != sizes[i]) {
			log_printf("sgx: struct %u is %lu bytes to the microkernel, %lu here\n", i, misc[8 + i], sizes[i]);
			info->structs_match = false;
		}
	return true;
}
