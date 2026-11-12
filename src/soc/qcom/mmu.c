/* The memory maps, from the regions in memmap.h.
 * APQ8026, flat:
 *   DRAM bank 0                              normal, write-back
 *   the gap between the banks (SMEM)         normal, uncached
 *   DRAM bank 1                              normal, write-back, except the
 *                                            GPU's ring buffer and command memory
 *                                            and the section holding the
 *                                            persistent log
 *   peripherals (APCS, clock/PMIC arbiter, TLMM, hardware mutexes, multimedia,
 *   IMEM) device, XN
 * APQ8009W, DRAM moved down by phys_offset: the same, with the carve-outs
 * left out but SMEM, and the peripherals moved up to IO_8009_BASE. */
#include "asteroid.h"

static void map_ram(uint32_t from, uint32_t to, uint32_t attr)
{
	mmu_map_to(from, to, from + phys_offset, attr);
}

void soc_memory_map(void)
{
	if (phys_offset) {
		map_ram(DRAM0_BASE, HOLE_8009_BASE, MMU_RAM);
		map_ram(SMEM_8009_BASE, SMEM_8009_END, MMU_RAM_UNCACHED);
		map_ram(HOLE_8009_END, DRAM_END, MMU_RAM);
		mmu_map_to(IO_8009_BASE, IO_8009_BASE + IO_8009_SIZE, 0, MMU_DEVICE);
	} else {
		mmu_map(DRAM0_BASE, DRAM0_END, MMU_RAM);
		mmu_map(DRAM0_END, DRAM1_BASE, MMU_RAM_UNCACHED);
		mmu_map(DRAM1_BASE, DRAM_END, MMU_RAM);
		mmu_map(0xf9000000, 0xfa000000, MMU_DEVICE);
		mmu_map(0xfc100000, 0xfc200000, MMU_DEVICE);
		mmu_map(0xfc400000, 0xfc500000, MMU_DEVICE);
		mmu_map(0xfd400000, 0xfd600000, MMU_DEVICE);
		mmu_map(0xfd800000, 0xfdc00000, MMU_DEVICE);
		mmu_map(0xfe800000, 0xfe900000, MMU_DEVICE);
	}
	map_ram(GPU_MEM_BASE, GPU_MEM_BASE + GPU_MEM_SIZE, MMU_RAM_UNCACHED);
	map_ram(RAMOOPS_BASE & ~0xfffffu, (RAMOOPS_BASE + RAMOOPS_SIZE + 0xfffffu) & ~0xfffffu, MMU_RAM_UNCACHED);
}
