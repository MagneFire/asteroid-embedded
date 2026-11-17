/* The memory map: the image runs from flash above the boot loader, and
 * everything it writes is in the 384 KiB of SRAM. The log region sits at
 * the top of SRAM, outside what start.S clears, so that it survives a
 * warm reset and the application processor can read it over SWD. The
 * settings take the two pages under IMG_END, inside what the vendor's
 * image covers; its own data from 0xec000 up is never written. The UI has no layers: it draws straight to
 * the panel, a few lines at a time. */
#ifndef MEMMAP_H
#define MEMMAP_H

#define FLASH_BASE		0x00000000
#define FLASH_END		0x00100000
#define FLASH_PAGE		0x2000
#define IMG_BASE		0x0000c000	/* below it the boot loader, never written */
#define IMG_END			0x000e8000
#define STORE_FLASH_BASE	0x000e8000	/* two pages of settings */
#define STORE_FLASH_SIZE	0x00004000
#define VENDOR_DATA_BASE	0x000ec000

#define SRAM_BASE		0x10000000
#define SRAM_END		0x10060000
#define KEPT_BASE		0x1005af00	/* the .kept section, kept across a warm reset */
#define KEPT_SIZE		0x00000100
#define RAMOOPS_BASE		0x1005b000	/* log and fault record, kept across a warm reset */
#define RAMOOPS_SIZE		0x00004800
#define SWD_BOX_BASE		0x1005f800	/* the console over SWD */
#define SWD_BOX_SIZE		0x00000800
#define DRAM0_END		KEPT_BASE	/* the stack ends below this */
#define RAMTEST_BASE		0x10010000	/* swdtool's scratch is below it */

#define UI_HEAP_SIZE		0x00020000
#define UI_LAYER_BASE		0
#define UI_LAYER_STRIDE		0
#define UI_LAYER_SIZE		0

#if !defined(LINKER_SCRIPT) && !defined(__ASSEMBLER__)
#include <stdint.h>
extern uint8_t ui_heap[];
#define UI_HEAP_BASE		((uintptr_t)ui_heap)
_Static_assert(IMG_END <= STORE_FLASH_BASE && STORE_FLASH_BASE + STORE_FLASH_SIZE <= VENDOR_DATA_BASE,
	       "store over the vendor's data");
_Static_assert(KEPT_BASE + KEPT_SIZE <= RAMOOPS_BASE, "kept section overlaps the log region");
_Static_assert(RAMOOPS_BASE + RAMOOPS_SIZE <= SWD_BOX_BASE && SWD_BOX_BASE + SWD_BOX_SIZE <= SRAM_END,
	       "log region overlaps the SWD box");
#endif
#endif
