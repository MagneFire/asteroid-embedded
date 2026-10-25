/* Calls into the secure world (TrustZone) in the legacy register
 * convention: r0 carries service, command and argument count, r1 points
 * at a context word, r2/r3 are the arguments, r0 returns the result. */
#include "asteroid.h"

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
