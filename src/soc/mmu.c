/* Flat 1:1 map with 1 MiB sections.
 *   DRAM bank 0 (0 - 0x02f00000)             normal, write-back
 *   0x02f00000 - 0x04700000 (bootloader hole, framebuffer) normal, uncached
 *   DRAM bank 1 (0x04700000 - 0x20000000)    normal, write-back, except the
 *                                            section holding the persistent log
 *   peripherals (APCS, clock/PMIC arbiter, TLMM, multimedia, IMEM) device, XN
 *   everything else unmapped: a wild pointer aborts instead of corrupting. */
#include "asteroid.h"

extern uint32_t __mmu_l1[];

#define SEC_WB	0x1c0eu		/* TEX=001 C=1 B=1, AP=11, section */
#define SEC_NC	0x1c02u		/* TEX=001 C=0 B=0: normal, non-cacheable */
#define SEC_DEV	0x0c16u		/* TEX=000 C=0 B=1, XN: device */

static void map(uint32_t from, uint32_t to, uint32_t attr)
{
	for (uint32_t a = from >> 20; a < (to >> 20); a++)
		__mmu_l1[a] = (a << 20) | attr;
}

void mmu_init(void)
{
	for (int i = 0; i < 4096; i++)
		__mmu_l1[i] = 0;
	map(0x00000000, 0x02f00000, SEC_WB);
	map(0x02f00000, 0x04700000, SEC_NC);
	map(0x04700000, 0x20000000, SEC_WB);
	map(RAMOOPS_BASE & ~0xfffffu, (RAMOOPS_BASE + RAMOOPS_SIZE + 0xfffffu) & ~0xfffffu, SEC_NC);
	map(0xf9000000, 0xfa000000, SEC_DEV);
	map(0xfc400000, 0xfc500000, SEC_DEV);
	map(0xfd500000, 0xfd600000, SEC_DEV);
	map(0xfd800000, 0xfdc00000, SEC_DEV);
	map(0xfe800000, 0xfe900000, SEC_DEV);

	dsb();
	uint32_t ttbr = (uint32_t)(uintptr_t)__mmu_l1;
	__asm__ volatile("mcr p15, 0, %0, c2, c0, 2" :: "r"(0));	/* TTBCR N=0 */
	__asm__ volatile("mcr p15, 0, %0, c2, c0, 0" :: "r"(ttbr));	/* TTBR0 */
	__asm__ volatile("mcr p15, 0, %0, c3, c0, 0" :: "r"(1));	/* DACR: domain 0 client */
	__asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0));	/* TLBIALL */
	dsb(); isb();
	uint32_t sctlr;
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
	sctlr |= (1 << 0) | (1 << 2) | (1 << 11) | (1 << 12);		/* M, C, Z, I */
	sctlr &= ~(1 << 1);						/* A: allow unaligned */
	__asm__ volatile("mcr p15, 0, %0, c1, c0, 0" :: "r"(sctlr));
	isb();
}

#define CACHE_LINE 64u
void dcache_clean_range(const void *addr, uint32_t size)
{
	if (!size) return;
	uintptr_t a = (uintptr_t)addr & ~(CACHE_LINE - 1), e = (uintptr_t)addr + size;
	dmb();
	for (; a < e; a += CACHE_LINE)
		__asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a));
	dsb();
}
void dcache_invalidate_range(const void *addr, uint32_t size)
{
	if (!size) return;
	uintptr_t a = (uintptr_t)addr & ~(CACHE_LINE - 1), e = (uintptr_t)addr + size;
	dmb();
	for (; a < e; a += CACHE_LINE)
		__asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a));
	dsb();
}
void dcache_clean_invalidate_range(const void *addr, uint32_t size)
{
	if (!size) return;
	uintptr_t a = (uintptr_t)addr & ~(CACHE_LINE - 1), e = (uintptr_t)addr + size;
	dmb();
	for (; a < e; a += CACHE_LINE)
		__asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a));
	dsb();
}
