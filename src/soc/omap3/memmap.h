/* The physical memory map: every fixed DRAM region the firmware uses, in
 * one place, checked against each other. The bootloader loads the image
 * where it likes, low in DRAM, with the device tree behind it; start.S
 * copies the image to IMG_BASE, and nothing below IMG_BASE is written
 * but the log, so the tree stays readable where it is. The vendor
 * kernel keeps its console log at RAMOOPS_BASE, and after a reset it
 * shows what it finds there, which is how a log gets off the watch. The
 * MMU table (mmu.c) and the linker script (ld/asteroid.ld.in,
 * preprocessed) are built from these names. */
#ifndef MEMMAP_H
#define MEMMAP_H

#define DRAM_BASE		0x80000000
#define DRAM_END		0xa0000000	/* 512 MiB */
#define RAMOOPS_BASE		0x83000000	/* log and fault record across a warm reset */
#define RAMOOPS_SIZE		0x00020000
#define IMG_BASE		0x88000000
#define IMG_END			0x8a000000
#define DRAM0_END		IMG_END		/* the stacks end below this */

#define FB_BASE			0x8a000000	/* two frame buffers */
#define FB_SIZE			0x00100000
#define UI_LAYER_BASE		0x8a100000	/* one RGBA layer per panel */
#define UI_LAYER_STRIDE		0x00100000
#define UI_LAYER_SIZE		0x00700000
#define SCREENSHOT_BASE		0x8a800000	/* layer copy plus JPEG */
#define SCREENSHOT_SIZE		0x00200000
#define UI_HEAP_BASE		0x8b000000	/* the Slint heap */
#define UI_HEAP_SIZE		0x04000000

#if !defined(LINKER_SCRIPT) && !defined(__ASSEMBLER__)
_Static_assert(RAMOOPS_BASE + RAMOOPS_SIZE <= IMG_BASE, "ramoops overlaps the image");
_Static_assert(IMG_END <= FB_BASE && FB_BASE + FB_SIZE <= UI_LAYER_BASE, "framebuffer overlaps");
_Static_assert(UI_LAYER_BASE + UI_LAYER_SIZE <= SCREENSHOT_BASE, "layers overlap the screenshot");
_Static_assert(SCREENSHOT_BASE + SCREENSHOT_SIZE <= UI_HEAP_BASE, "screenshot overlaps the UI heap");
_Static_assert(UI_HEAP_BASE + UI_HEAP_SIZE <= DRAM_END, "UI heap outside DRAM");
#endif
#endif
