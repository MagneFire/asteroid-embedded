/* sgx.c itself on the host: its start, stop and failures against a fake
 * register file and a fake microkernel that answers the commands it is
 * kicked for, or does not when a case says so. Every failure must leave
 * the GPU halted (held in reset while it still had its clocks, then no
 * clocks and no power) within a bounded time with the watchdog fed, and
 * the next start must build everything again; a clean stop and start
 * keeps what was built. The image needs the vendor's libsrv_init.so
 * (SGX_LIBSRV_INIT); without it only the cases before the image run. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "gpu/sgx.h"
#include "gpu/sgx_image.h"

/* unistd.h would clash with the firmware's reboot(). */
pid_t fork(void);
void _exit(int status) __attribute__((noreturn));

#define R(off)		(SGX_BASE + (off))
#define CLKGATESTATUS	R(0x004)
#define SOFT_RESET	R(0x080)
#define EVENT_KICK	R(0xac8)
#define BIF_CTRL	R(0xc00)
#define BIF_INT_STAT	R(0xc04)
#define BIF_FAULT	R(0xc08)
#define BIF_DIR_LIST	R(0xc84)
#define BIF_MEM_REQ	R(0xca8)
#define OCP_IRQ_CLR	R(0xfe50)
#define INIT_COMPLETE	1u
#define POWER_DONE	8u
#define MISC_READY	1u

static int failures;
#define CHECK(cond, ...) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
			failures++; \
		} \
	} while (0)

uint8_t *sgx_host_pool;
static uint8_t *lib, *scratch;
static long lib_size;
static struct sgx_image ref;
static unsigned arg_word;

/* --- time and the watchdog */

static uint32_t now, fed, max_gap;
uint32_t time_us(void) { return now += 10; }
uint32_t time_ms(void) { return now / 1000; }
void delay_us(uint32_t us) { now += us; }
void wdt_feed(void)
{
	if (now - fed > max_gap)
		max_gap = now - fed;
	fed = now;
}
static uint32_t gap(void) { return now - fed > max_gap ? now - fed : max_gap; }

/* --- the rest of the firmware */

static bool hide_lib, irq_masked, irq_on;
static unsigned fw_reads, nlogs;
void irq_attach(unsigned id, void (*fn)(void)) { irq_on = true; irq_masked = false; }
void irq_mask(unsigned id) { irq_masked = true; }
uint32_t irq_save(void) { return 0; }
void irq_restore(uint32_t cpsr) {}
void dsb(void) {}
void dmb(void) {}

int firmware_read(const char *path, uint32_t off, void *buf, uint32_t len)
{
	fw_reads++;
	if (!lib || hide_lib || off + len > (uint32_t)lib_size)
		return -1;
	memcpy(buf, lib + off, len);
	return (int)len;
}

void log_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("  log: ");
	vprintf(fmt, ap);
	va_end(ap);
	nlogs++;
}

/* --- the GPU */

static uint32_t sgx_regs[0x10000 / 4], cm[0x100 / 4], prm[0x100 / 4];
/* What a case breaks. */
static bool no_power, init_hangs, power_hangs, gate_stuck, req_stuck, faults_stick;
static unsigned faults;
static unsigned off_access, resets_on, resets_off, kicks, drains;
static uint32_t cmd_cache[64];
static unsigned cmd_type[64], ncmds;

static bool clocked(void)
{
	return !no_power && (prm[PM_PWSTCTRL / 4] & 3) == 3 && cm[CM_FCLKEN / 4] & 2 && cm[CM_ICLKEN / 4] & 1;
}

static uint32_t *reg(uint32_t a)
{
	static uint32_t junk;
	if (a >= SGX_BASE && a < SGX_BASE + sizeof sgx_regs) {
		off_access += !clocked();
		return &sgx_regs[(a - SGX_BASE) / 4];
	}
	if (a >= CM_SGX && a < CM_SGX + sizeof cm)
		return &cm[(a - CM_SGX) / 4];
	if (a >= PRM_SGX && a < PRM_SGX + sizeof prm)
		return &prm[(a - PRM_SGX) / 4];
	printf("FAIL access to %08x\n", a);
	failures++;
	return &junk;
}

uint32_t readl(uint32_t a)
{
	if (a == PRM_SGX + PM_PWSTST)
		return no_power ? 0 : prm[PM_PWSTCTRL / 4] & 3;
	uint32_t *r = reg(a);
	if (a == BIF_MEM_REQ)
		return req_stuck;
	if (a == BIF_INT_STAT)
		return faults ? 1 : 0;
	if (a == BIF_FAULT)
		return 0x00400000u * faults + 0x1000;
	if (a == CLKGATESTATUS)
		return gate_stuck;
	return *r;
}

static unsigned type_of(const volatile uint32_t *c)
{
	for (unsigned t = 0; t < SGX_CMDS; t++) {
		uint32_t w[8];
		sgx_command_words(w, t, 0, 0);
		if (w[0] == c[0])
			return t;
	}
	return SGX_CMDS;
}

/* The microkernel: started by the first kick, then a command for each. */
static void microkernel(void)
{
	kicks++;
	struct sgx_mem *m = sgx_memory();
	if (!m) {
		printf("FAIL kick without an image\n");
		failures++;
		return;
	}
	volatile uint32_t *host = sgx_ptr(m, ref.host_ctl), *ccb = sgx_ptr(m, ref.ccb), *ctl = sgx_ptr(m, ref.ccb_ctl);
	if (!(host[0] & INIT_COMPLETE)) {
		if (!init_hangs)
			host[0] |= INIT_COMPLETE;
		return;
	}
	while (ctl[1] != ctl[0]) {
		volatile uint32_t *c = ccb + ctl[1] * 8;
		unsigned t = type_of(c);
		cmd_type[ncmds % 64] = t;
		cmd_cache[ncmds++ % 64] = c[1];
		if (t == SGX_CMD_POWER) {
			if (power_hangs)
				return;
			host[1] |= POWER_DONE;
		} else if (t == SGX_CMD_GETMISCINFO) {
			*(volatile uint32_t *)sgx_ptr(m, c[arg_word]) |= MISC_READY;
		}
		ctl[1] = (ctl[1] + 1) & 0xff;
	}
}

void writel(uint32_t v, uint32_t a)
{
	*reg(a) = v;
	if (a == SOFT_RESET && v == 0x7f) {
		resets_on += clocked();
		resets_off += !clocked();
	}
	if (a == BIF_CTRL && v == 8) {
		drains++;
		if (faults && !faults_stick)
			faults--;
	}
	if (a == EVENT_KICK)
		microkernel();
}

static bool off(void) { return !cm[CM_FCLKEN / 4] && !cm[CM_ICLKEN / 4] && !(prm[PM_PWSTCTRL / 4] & 3); }

/* After every failure: halted, with its clocks while held in reset. */
static void check_halted(const char *what, uint32_t t0, uint32_t limit_us)
{
	CHECK(!sgx_running() && sgx_failed(), "%s: running %d, failed %d", what, sgx_running(), sgx_failed());
	CHECK(off() && irq_masked && sgx_regs[(OCP_IRQ_CLR - SGX_BASE) / 4] == 1, "%s: not off: fclk %x iclk %x pwst %x irq %d",
	      what, cm[CM_FCLKEN / 4], cm[CM_ICLKEN / 4], prm[PM_PWSTCTRL / 4], irq_masked);
	CHECK(resets_on && !resets_off, "%s: resets with clocks %u, without %u", what, resets_on, resets_off);
	CHECK(now - t0 < limit_us, "%s: %u us", what, now - t0);
	CHECK(!off_access, "%s: %u register accesses without clocks", what, off_access);
	CHECK(gap() < 300000, "%s: the watchdog went %u us unfed", what, gap());
}

/* --- cases */

static void normal(void)
{
	faults = 5;
	CHECK(sgx_start() && sgx_running() && !sgx_failed() && sgx_generation() == 1 && irq_on, "start");
	CHECK(!faults && drains == 6 && sgx_regs[(BIF_DIR_LIST - SGX_BASE) / 4] == SGX_MEM_BASE, "first reset: %u faults, %u drains",
	      faults, drains);
	struct sgx_info i;
	CHECK(sgx_query(&i), "query");
	unsigned reads = fw_reads, d = drains;
	CHECK(sgx_stop() && !sgx_running() && !sgx_failed() && off() && irq_masked, "stop");
	CHECK(cmd_type[(ncmds - 1) % 64] == SGX_CMD_POWER, "the last command a power-off");
	CHECK(sgx_start() && fw_reads == reads && sgx_generation() == 1 && drains == d + 1, "start again: %u reads, %u drains",
	      fw_reads - reads, drains - d);
	CHECK(sgx_stop() && sgx_stop(), "stop twice");
	CHECK(!resets_off && !off_access && gap() < 300000, "%u resets without clocks, %u accesses, %u us unfed", resets_off,
	      off_access, gap());
}

/* Then the start after a failure, from scratch: the image read again, the
 * first reset's drain run again, a new generation. */
static void retry(const char *what)
{
	unsigned reads = fw_reads;
	uint32_t g = sgx_generation();
	init_hangs = power_hangs = gate_stuck = req_stuck = faults_stick = hide_lib = no_power = false;
	faults = 2;
	resets_on = 0;
	CHECK(sgx_start() && !sgx_failed() && fw_reads > reads && sgx_generation() == g + 1 && !faults,
	      "%s: start after it: %u reads, generation %u, %u faults", what, fw_reads - reads, sgx_generation(), faults);
	CHECK(sgx_stop() && off(), "%s: stop after it", what);
}

static void init_timeout(void)
{
	init_hangs = true;
	uint32_t t0 = now;
	CHECK(!sgx_start(), "start");
	check_halted("init timeout", t0, 1500000);
	retry("init timeout");
}

static void power_timeout(void)
{
	CHECK(sgx_start(), "start");
	power_hangs = true;
	resets_on = 0;
	uint32_t t0 = now;
	CHECK(!sgx_stop(), "stop");
	check_halted("power timeout", t0, 1500000);
	retry("power timeout");
}

static void gate_timeout(void)
{
	CHECK(sgx_start(), "start");
	gate_stuck = true;
	resets_on = 0;
	uint32_t t0 = now;
	CHECK(!sgx_stop(), "stop");
	check_halted("clock gating timeout", t0, 1500000);
	retry("clock gating timeout");
}

static void reset_timeout(void)
{
	req_stuck = true;
	uint32_t t0 = now;
	CHECK(!sgx_start(), "start");
	check_halted("requests that stay out", t0, 400000);
	retry("requests that stay out");
}

static void faults_forever(void)
{
	faults = 1;
	faults_stick = true;
	uint32_t t0 = now;
	CHECK(!sgx_start(), "start");
	check_halted("faults that come back", t0, 400000);
	retry("faults that come back");
}

static void halted_running(void)
{
	CHECK(sgx_start(), "start");
	uint32_t t0 = now;
	sgx_halt();
	check_halted("halt", t0, 100000);
	sgx_halt();
	CHECK(!off_access, "a second halt");
	retry("halt");
}

/* A data invalidate never goes with a page-table one. */
static void inval_data(void)
{
	CHECK(sgx_start(), "start");
	unsigned n = ncmds;
	CHECK(sgx_submit_ta(0x1000, SGX_CACHE_INVAL_DATA | SGX_CACHE_INVAL_PT), "TA");
	CHECK(sgx_submit_ta(0x1000, 0), "TA");
	CHECK(sgx_submit_ta(0x1000, SGX_CACHE_INVAL_DATA), "TA");
	CHECK(ncmds == n + 3 && cmd_cache[n % 64] == (SGX_CACHE_INVAL_PT | SGX_CACHE_INVAL_PD) &&
	      cmd_cache[(n + 1) % 64] == SGX_CACHE_INVAL_DATA && cmd_cache[(n + 2) % 64] == SGX_CACHE_INVAL_DATA,
	      "cache words %x %x %x", cmd_cache[n % 64], cmd_cache[(n + 1) % 64], cmd_cache[(n + 2) % 64]);
}

static void no_library(void)
{
	hide_lib = true;
	uint32_t t0 = now;
	CHECK(!sgx_start() && !sgx_memory(), "start");
	check_halted("no library", t0, 300000);
	CHECK(!kicks, "kicked without an image");
	if (lib)
		retry("no library");
}

static void no_domain(void)
{
	no_power = true;
	uint32_t t0 = now;
	CHECK(!sgx_start(), "start");
	CHECK(!sgx_running() && sgx_failed() && !cm[CM_FCLKEN / 4] && !(prm[PM_PWSTCTRL / 4] & 3), "off");
	CHECK(!off_access && !resets_on && !resets_off && now - t0 < 200000, "%u accesses, %u resets, %u us", off_access,
	      resets_on + resets_off, now - t0);
	sgx_halt();
	CHECK(!off_access, "a halt without power");
	if (lib)
		retry("no power");
}

static void run(const char *name, void (*fn)(void))
{
	fflush(stdout);
	pid_t pid = fork();
	if (!pid) {
		failures = 0;
		fn();
		fflush(stdout);
		_exit(failures ? 1 : 0);
	}
	int st;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) {
		printf("FAIL %s\n", name);
		failures++;
	} else
		printf("%s: ok\n", name);
}

static bool read_lib(void *ctx, uint32_t off, void *buf, uint32_t len)
{
	return firmware_read(NULL, off, buf, len) == (int)len;
}

int main(void)
{
	sgx_host_pool = aligned_alloc(4096, SGX_MEM_SIZE);
	scratch = aligned_alloc(4096, SGX_MEM_SIZE);
	uint32_t w[8];
	sgx_command_words(w, SGX_CMD_GETMISCINFO, 0, 0x12345678);
	for (unsigned i = 0; i < 8; i++)
		if (w[i] == 0x12345678)
			arg_word = i;
	const char *path = getenv("SGX_LIBSRV_INIT");
	FILE *f = path ? fopen(path, "rb") : NULL;
	if (f) {
		fseek(f, 0, SEEK_END);
		lib_size = ftell(f);
		rewind(f);
		lib = malloc((size_t)lib_size);
		if (fread(lib, 1, (size_t)lib_size, f) != (size_t)lib_size)
			return 2;
		fclose(f);
		struct sgx_mem m;
		sgx_mem_init(&m, scratch, SGX_MEM_BASE, SGX_MEM_SIZE);
		if (!sgx_image_build(&m, &ref, read_lib, NULL)) {
			printf("FAIL no image from %s\n", path);
			return 1;
		}
	}
	run("no power", no_domain);
	run("no library", no_library);
	if (!lib) {
		printf("sgx_test: SKIP: start, stop and failures with the microkernel not run, no vendor library "
		       "(SGX_LIBSRV_INIT)\n");
		if (getenv("SGX_REQUIRE_REFERENCE"))
			failures++;
	} else {
		run("start and stop", normal);
		run("init timeout", init_timeout);
		run("power timeout", power_timeout);
		run("clock gating timeout", gate_timeout);
		run("reset with requests that stay out", reset_timeout);
		run("reset with faults that come back", faults_forever);
		run("halt while running", halted_running);
		run("data invalidate", inval_data);
	}
	if (failures)
		return 1;
	printf("sgx_test: ok\n");
	return 0;
}
