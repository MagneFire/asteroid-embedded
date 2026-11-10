/* The APQ8026's memory map, from the regions in memmap.h:
 *   DRAM bank 0                              normal, write-back
 *   the gap between the banks (SMEM)         normal, uncached
 *   DRAM bank 1                              normal, write-back, except the
 *                                            GPU's ring buffer and command memory
 *                                            and the section holding the
 *                                            persistent log
 *   peripherals (APCS, clock/PMIC arbiter, TLMM, hardware mutexes, multimedia,
 *   IMEM) device, XN */
#include "asteroid.h"

void soc_memory_map(void)
{
	mmu_map(DRAM0_BASE, DRAM0_END, MMU_RAM);
	mmu_map(DRAM0_END, DRAM1_BASE, MMU_RAM_UNCACHED);
	mmu_map(DRAM1_BASE, DRAM_END, MMU_RAM);
	mmu_map(GPU_MEM_BASE, GPU_MEM_BASE + GPU_MEM_SIZE, MMU_RAM_UNCACHED);
	mmu_map(RAMOOPS_BASE & ~0xfffffu, (RAMOOPS_BASE + RAMOOPS_SIZE + 0xfffffu) & ~0xfffffu, MMU_RAM_UNCACHED);
	mmu_map(0xf9000000, 0xfa000000, MMU_DEVICE);
	mmu_map(0xfc100000, 0xfc200000, MMU_DEVICE);
	mmu_map(0xfc400000, 0xfc500000, MMU_DEVICE);
	mmu_map(0xfd400000, 0xfd600000, MMU_DEVICE);
	mmu_map(0xfd800000, 0xfdc00000, MMU_DEVICE);
	mmu_map(0xfe800000, 0xfe900000, MMU_DEVICE);
}
