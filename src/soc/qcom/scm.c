/* Calls into the secure world (TrustZone) in the legacy register
 * convention: r0 carries service, command and argument count, r1 points
 * at a context word, r2/r3 are the arguments, r0 returns the result. */
#include <stddef.h>
#include "asteroid.h"
#include "config.h"

#define SCM_CLASS_REGISTER	(0x2 << 8)
#define SCM_MASK_IRQS		BIT(5)
#define SCM_ATOMIC(svc, cmd, n)	((((((svc) & 0x3f) << 10) | ((cmd) & 0x3ff)) << 12) | \
				 SCM_CLASS_REGISTER | SCM_MASK_IRQS | ((n) & 0xf))
#define SCM_SVC_BOOT		0x01
#define SCM_WDOG_DEBUG_BOOT_PART 0x09
#define SCM_DLOAD_CMD		0x10

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

/* Without this a watchdog bite stops in the secure debug image instead
 * of resetting the chip, and a PS_HOLD drop may be ignored. The second
 * call clears the secure world's own download-mode flag. */
int scm_wdog_debug_disable(void)
{
	int ret = scm_call2(SCM_SVC_BOOT, SCM_WDOG_DEBUG_BOOT_PART, 1, 0);
	scm_call2(SCM_SVC_BOOT, SCM_DLOAD_CMD, 0, 0);
	return ret;
}

/* The buffer-based convention: a command block in memory holds the
 * request and room for the response, r2 carries its address, and the
 * call is repeated while the secure world reports it interrupted. */
#define SCM_SVC_MP		0x0c
#define SCM_RESTORE_SEC_CFG	0x02
#define SCM_INTERRUPTED		1
#define SCM_WAIT_MS		100
#define SCM_RESP_HDR		12

static struct {
	uint32_t len, buf_offset, resp_hdr_offset, id;
	uint32_t req[2];
	uint32_t resp_len, resp_buf_offset, is_complete;
	uint32_t resp;
} __attribute__((aligned(64))) scm_cmd;

/* After its power domain collapsed, a device behind a TZ-managed SMMU
 * (the display is device 1) can't be reached until the secure world has
 * restored its configuration, with the SMMU's clocks running. The second
 * word is a context bank, 0 for the display. */
int scm_restore_sec_cfg(uint32_t device, uint32_t bank)
{
	scm_cmd = (typeof(scm_cmd)){ .len = sizeof scm_cmd, .buf_offset = 16, .resp_hdr_offset = 24,
				     .id = SCM_SVC_MP << 10 | SCM_RESTORE_SEC_CFG, .req = { device, bank } };
	dcache_clean_invalidate_range(&scm_cmd, sizeof scm_cmd);
#if defined(TEST_SCM_FAIL)
	static bool injected;
	if (!injected && time_ms() > 60000) {
		injected = true;
		return -1;
	}
#endif
	uint32_t context = 0, t0 = time_ms();
	register uint32_t r0 __asm__("r0");
	do {
		register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)&context;
		register uint32_t r2 __asm__("r2") = dma_addr(&scm_cmd);
		r0 = SCM_INTERRUPTED;
		__asm__ volatile("smc #0" : "+r"(r0) : "r"(r1), "r"(r2) : "r3", "memory");
		if (r0 == SCM_INTERRUPTED && time_ms() - t0 > SCM_WAIT_MS)
			return -2;
	} while (r0 == SCM_INTERRUPTED);
	if (r0)
		return (int)r0;
	const volatile uint32_t *complete = &scm_cmd.is_complete;
	for (;;) {
		dcache_invalidate_range(&scm_cmd, sizeof scm_cmd);
		bool done = *complete;
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
		if (time_ms() - t0 > SCM_WAIT_MS)
			return -3;
	}
	/* The response follows its header unless the secure world says
	 * otherwise. */
	uint32_t at = offsetof(typeof(scm_cmd), resp_len) +
		      (scm_cmd.resp_buf_offset ? scm_cmd.resp_buf_offset : SCM_RESP_HDR);
	if (at % 4 || at + 4 > sizeof scm_cmd)
		return -4;
	return *(const uint32_t *)((const char *)&scm_cmd + at) ? -1 : 0;
}
