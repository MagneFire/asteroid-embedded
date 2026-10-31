/* The physical memory map: every fixed DRAM region the firmware uses, in
 * one place, checked against each other. The bootloader loads the image
 * at IMG_BASE and puts the device tree at IMG_END; the framebuffer it
 * leaves on is at FB_BASE; everything else is ours to choose. The MMU
 * table (mmu.c) and the linker script (ld/asteroid.ld.in, preprocessed)
 * are built from these names, so a change here moves everything. */
#ifndef MEMMAP_H
#define MEMMAP_H

#define DRAM0_BASE		0x00000000
#define DRAM0_END		0x02f00000	/* first bank: image, stacks, MMU table */
#define IMG_BASE		0x00008000
#define IMG_END			0x01e00000	/* the bootloader's device tree */
#define SMEM_BASE		0x03000000	/* shared memory with the RPM */
#define DRAM1_BASE		0x04700000	/* second bank */
#define DRAM_END		0x20000000	/* 512 MiB */

#define FB_BASE			0x05000000	/* two inherited frame buffers */
#define FB_SIZE			0x01000000
#define UI_HEAP_BASE		0x06000000	/* the Slint heap */
#define UI_HEAP_SIZE		0x04000000
#define UI_LAYER_BASE		0x0a000000	/* one RGBA layer per panel */
#define UI_LAYER_STRIDE		0x00100000
#define UI_LAYER_SIZE		0x00700000
#define SCREENSHOT_BASE		0x0b000000	/* layer copy plus JPEG */
#define SCREENSHOT_SIZE		0x00200000
#define GPU_MEM_BASE		0x1e000000	/* ring buffer, command and vertex memory */
#define GPU_MEM_SIZE		0x00f00000
#define RAMOOPS_BASE		0x1ef80000	/* log and fault records across a warm reset */
#define RAMOOPS_SIZE		0x00080000

#ifndef LINKER_SCRIPT
_Static_assert(IMG_BASE < IMG_END && IMG_END <= DRAM0_END, "image outside bank 0");
_Static_assert(DRAM0_END <= SMEM_BASE && SMEM_BASE < DRAM1_BASE, "SMEM outside the gap");
_Static_assert(DRAM1_BASE <= FB_BASE, "framebuffer outside bank 1");
_Static_assert(FB_BASE + FB_SIZE <= UI_HEAP_BASE, "framebuffer overlaps the UI heap");
_Static_assert(UI_HEAP_BASE + UI_HEAP_SIZE <= UI_LAYER_BASE, "UI heap overlaps the layers");
_Static_assert(UI_LAYER_BASE + UI_LAYER_SIZE <= SCREENSHOT_BASE, "layers overlap the screenshot");
_Static_assert(SCREENSHOT_BASE + SCREENSHOT_SIZE <= GPU_MEM_BASE, "screenshot overlaps GPU memory");
_Static_assert(GPU_MEM_BASE + GPU_MEM_SIZE <= RAMOOPS_BASE, "GPU memory overlaps ramoops");
_Static_assert(RAMOOPS_BASE + RAMOOPS_SIZE <= DRAM_END, "ramoops outside DRAM");
_Static_assert((GPU_MEM_BASE & 0xfffff) == 0 && (GPU_MEM_SIZE & 0xfffff) == 0, "GPU memory not section aligned");
#endif
#endif
