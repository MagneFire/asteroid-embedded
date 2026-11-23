/* Calls into the secure world (TrustZone) in the legacy register
 * convention: r0 carries service, command and argument count, r1 points
 * at a context word, r2/r3 are the arguments, r0 returns the result. */
#include <stddef.h>
#include <string.h>
#include "asteroid.h"
#include "config.h"

#define SCM_CLASS_REGISTER	(0x2 << 8)
#define SCM_MASK_IRQS		BIT(5)
#define SCM_ATOMIC(svc, cmd, n)	((((((svc) & 0x3f) << 10) | ((cmd) & 0x3ff)) << 12) | \
				 SCM_CLASS_REGISTER | SCM_MASK_IRQS | ((n) & 0xf))
#define SCM_SVC_BOOT		0x01
#define SCM_WDOG_DEBUG_BOOT_PART 0x09
#define SCM_DLOAD_CMD		0x10
#define SCM_SVC_IO		0x05
#define SCM_IO_WRITE		0x02

int scm_call2(uint32_t svc, uint32_t cmd, uint32_t arg1, uint32_t arg2)
{
	uint32_t context;
	register uint32_t r0 __asm__("r0") = SCM_ATOMIC(svc, cmd, 2);
	register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)&context;
	register uint32_t r2 __asm__("r2") = arg1;
	register uint32_t r3 __asm__("r3") = arg2;
	__asm__ volatile("smc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3) : "memory");
	return (int)r0;
}

/* The secure world's download-mode flag. A TZ without the call has it in
 * a TCSR register only it can write. */
int soc_dload_mode(bool on)
{
	int r = scm_call2(SCM_SVC_BOOT, SCM_DLOAD_CMD, on ? SCM_DLOAD_CMD : 0, 0);
	if (r && chip->tcsr_boot_misc)
		r = scm_call2(SCM_SVC_IO, SCM_IO_WRITE, chip->tcsr_boot_misc, on ? SCM_DLOAD_CMD : 0);
	return r;
}

/* Without this a watchdog bite stops in the secure debug image instead
 * of resetting the chip, and a PS_HOLD drop may be ignored. Download mode
 * is cleared as well. */
int scm_wdog_debug_disable(void)
{
	int ret = scm_call2(SCM_SVC_BOOT, SCM_WDOG_DEBUG_BOOT_PART, 1, 0);
	soc_dload_mode(false);
	return ret;
}

/* The buffer-based convention: a command block in memory holds the
 * request and room for the response, r2 carries its address, and the
 * call is repeated while the secure world reports it interrupted. */
#define SCM_SVC_INFO		0x06
#define SCM_IS_CALL_AVAIL	0x01
#define SCM_SVC_MP		0x0c
#define SCM_RESTORE_SEC_CFG	0x02
#define SCM_BOOT_ADDR		0x01
#define SCM_BOOT_ADDR_MC	0x11
#define SCM_INTERRUPTED		1
#define SCM_WAIT_MS		100
#define SCM_CMD_HDR		16
#define SCM_RESP_HDR		12
#define SCM_REQ_MAX		6

static uint32_t scm_cmd_buf[16] __attribute__((aligned(64)));

static int scm_call_in(uint32_t *scm_cmd, uint32_t svc, uint32_t cmd, const uint32_t *req, unsigned n, uint32_t *resp,
		       uint32_t wait_ms)
{
	if (n > SCM_REQ_MAX)
		return -5;
	unsigned resp_hdr = SCM_CMD_HDR + 4 * n;
	memset(scm_cmd, 0, sizeof scm_cmd_buf);
	scm_cmd[0] = resp_hdr + SCM_RESP_HDR + 4;
	scm_cmd[1] = SCM_CMD_HDR;
	scm_cmd[2] = resp_hdr;
	scm_cmd[3] = svc << 10 | cmd;
	memcpy(&scm_cmd[SCM_CMD_HDR / 4], req, 4 * n);
	dcache_clean_invalidate_range(scm_cmd, sizeof scm_cmd_buf);
	uint32_t context = 0, t0 = time_ms();
	register uint32_t r0 __asm__("r0");
	do {
		register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)&context;
		register uint32_t r2 __asm__("r2") = dma_addr(scm_cmd);
		r0 = SCM_INTERRUPTED;
		__asm__ volatile("smc #0" : "+r"(r0) : "r"(r1), "r"(r2) : "r3", "memory");
		if (r0 == SCM_INTERRUPTED && time_ms() - t0 > wait_ms)
			return -2;
		if (wait_ms > SCM_WAIT_MS)
			wdt_feed();
	} while (r0 == SCM_INTERRUPTED);
	if (r0)
		return (int)r0;
	const volatile uint32_t *rh = &scm_cmd[resp_hdr / 4];
	for (;;) {
		dcache_invalidate_range(scm_cmd, sizeof scm_cmd_buf);
		bool done = rh[2];
#if defined(TEST_SCM_SLOW)
		/* Once, after a minute: complete only after 20 ms. */
		static bool slowed;
		if (!slowed && t0 > 60000)
			done = time_ms() - t0 >= 20 && (slowed = true);
#elif defined(TEST_SCM_HANG)
		if (t0 > 60000)
			done = false;
#endif
		if (done)
			break;
		if (time_ms() - t0 > wait_ms)
			return -3;
		if (wait_ms > SCM_WAIT_MS)
			wdt_feed();
	}
	/* The response follows its header unless the secure world says
	 * otherwise. */
	uint32_t at = resp_hdr + (rh[1] ? rh[1] : SCM_RESP_HDR);
	if (at % 4 || at + 4 > sizeof scm_cmd_buf)
		return -4;
	if (resp)
		*resp = scm_cmd[at / 4];
	return 0;
}

static int scm_call_buf(uint32_t svc, uint32_t cmd, const uint32_t *req, unsigned n, uint32_t *resp)
{
	return scm_call_in(scm_cmd_buf, svc, cmd, req, n, resp, SCM_WAIT_MS);
}

/* After its power domain collapsed, a device behind a TZ-managed SMMU
 * (the display is device 1) can't be reached until the secure world has
 * restored its configuration, with the SMMU's clocks running. The second
 * word is a context bank, 0 for the display. */
int scm_restore_sec_cfg(uint32_t device, uint32_t bank)
{
#if defined(TEST_SCM_FAIL)
	static bool injected;
	if (!injected && time_ms() > 60000) {
		injected = true;
		return -1;
	}
#endif
	const uint32_t req[2] = { device, bank };
	uint32_t resp = 0;
	int r = scm_call_buf(SCM_SVC_MP, SCM_RESTORE_SEC_CFG, req, 2, &resp);
	return r ? r : resp ? -1 : 0;
}

static bool scm_call_available(uint32_t svc, uint32_t cmd)
{
	uint32_t req = svc << 10 | cmd, resp = 0;
	return !scm_call_buf(SCM_SVC_INFO, SCM_IS_CALL_AVAIL, &req, 1, &resp) && resp;
}

/* Where the secure world starts the cores in cpus (a bit each) on a
 * cold or a warm boot. The APQ8026's takes the atomic call, the
 * APQ8009W's the buffer-based ones its kernel uses, with the
 * multi-cluster one where it exists. */
int scm_set_boot_addr(uint32_t phys, uint32_t cpus, bool warm)
{
	static const uint8_t cold_flag[4] = { 0, 0x01, 0x08, 0x20 }, warm_flag[4] = { 0x04, 0x02, 0x10, 0x40 };
	uint32_t flags = 0;
	for (unsigned cpu = 0; cpu < 4; cpu++)
		if (cpus & BIT(cpu))
			flags |= warm ? warm_flag[cpu] : cold_flag[cpu];
	if (chip != &chip_8009)
		return scm_call2(SCM_SVC_BOOT, SCM_BOOT_ADDR, flags, phys);
	static int mc = -1;
	if (mc < 0) {
		mc = scm_call_available(SCM_SVC_BOOT, SCM_BOOT_ADDR_MC);
		log_printf("scm: multi-cluster boot address %s\n", mc ? "available" : "not available");
	}
	if (mc) {
		const uint32_t req[6] = { phys, cpus, BIT(0), BIT(0), ~0u, warm ? 0x04 : 0x02 };
		return scm_call_buf(SCM_SVC_BOOT, SCM_BOOT_ADDR_MC, req, 6, NULL);
	}
	const uint32_t req[2] = { flags, phys };
	return scm_call_buf(SCM_SVC_BOOT, SCM_BOOT_ADDR, req, 2, NULL);
}

/* The peripheral image loader in the secure world: it takes the image's
 * headers and hash table, the region the image goes to, and then checks
 * the loaded segments and starts the processor. Checking several MB of
 * firmware takes much longer than the other calls. Each returns the call's
 * error or else the secure world's answer, 0 for success. */
#define SCM_SVC_PIL		0x02
#define PAS_INIT_IMAGE		1
#define PAS_MEM_SETUP		2
#define PAS_AUTH_AND_RESET	5
#define PAS_SHUTDOWN		6
#define PAS_WAIT_MS		10000
#define PAS_STUCK		-7

/* A call given up on may still be running in the secure world with its
 * buffer, so the loader gets a buffer of its own and no further call
 * once one timed out. */
static uint32_t pas_cmd[16] __attribute__((aligned(64)));
static bool pas_stuck;

static int pas_call(uint32_t cmd, const uint32_t *req, unsigned n)
{
	if (pas_stuck)
		return PAS_STUCK;
	uint32_t resp = ~0u;
	int r = scm_call_in(pas_cmd, SCM_SVC_PIL, cmd, req, n, &resp, PAS_WAIT_MS);
	if (r == -2 || r == -3)
		pas_stuck = true;
	return r ? r : (int)resp;
}

bool scm_pas_stuck(void) { return pas_stuck; }

int scm_pas_init_image(uint32_t pas_id, uint32_t metadata_phys)
{
	const uint32_t req[2] = { pas_id, metadata_phys };
	return pas_call(PAS_INIT_IMAGE, req, 2);
}

int scm_pas_mem_setup(uint32_t pas_id, uint32_t start_phys, uint32_t size)
{
	const uint32_t req[3] = { pas_id, start_phys, size };
	return pas_call(PAS_MEM_SETUP, req, 3);
}

int scm_pas_auth_and_reset(uint32_t pas_id) { return pas_call(PAS_AUTH_AND_RESET, &pas_id, 1); }

int scm_pas_shutdown(uint32_t pas_id) { return pas_call(PAS_SHUTDOWN, &pas_id, 1); }
