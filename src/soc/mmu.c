/* Flat 1:1 map with 1 MiB sections. The SoC lists its DRAM and its
 * peripherals (soc_memory_map); everything else is unmapped, so a wild
 * pointer aborts instead of corrupting. */
#include "asteroid.h"

extern uint32_t __mmu_l1[];

void mmu_map(uint32_t from, uint32_t to, uint32_t attr)
{
	for (uint32_t a = from >> 20; a < (to >> 20); a++)
		__mmu_l1[a] = (a << 20) | attr;
}

void mmu_init(void)
{
	for (int i = 0; i < 4096; i++)
		__mmu_l1[i] = 0;
	soc_memory_map();

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
