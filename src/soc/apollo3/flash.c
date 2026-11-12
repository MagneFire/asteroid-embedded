/* Internal flash through the boot ROM's helpers. The flash cannot be
 * read while one of its instances is being written, and code runs from
 * it, so interrupts stay off for the ROM's call; the helpers run from
 * ROM themselves. Only the store's pages are taken: everything below
 * them is the image or the boot loader. */
#include "asteroid.h"

#define PROGRAM_KEY		0x12344321u
#define ROM_PAGE_ERASE		0x08000051u
#define ROM_PROGRAM_MAIN	0x08000055u
#define INSTANCE_SIZE		0x80000u

typedef int (*page_erase_fn)(uint32_t key, uint32_t inst, uint32_t page);
typedef int (*program_fn)(uint32_t key, const uint32_t *src, uint32_t *dst, uint32_t words);

static bool in_store(uint32_t addr, uint32_t len)
{
	return addr >= STORE_FLASH_BASE && len <= STORE_FLASH_SIZE && addr - STORE_FLASH_BASE <= STORE_FLASH_SIZE - len;
}

int flash_erase_page(uint32_t addr)
{
	if ((addr & (FLASH_PAGE - 1)) || !in_store(addr, FLASH_PAGE))
		return -1;
	uint32_t flags = irq_save();
	int r = ((page_erase_fn)ROM_PAGE_ERASE)(PROGRAM_KEY, addr / INSTANCE_SIZE, addr % INSTANCE_SIZE / FLASH_PAGE);
	irq_restore(flags);
	return r ? -2 : 0;
}

/* The ROM reads past the source's last word, so a source that ends at
 * the top of SRAM would be read outside it; the stack and data never
 * reach there. */
int flash_program(uint32_t addr, const uint32_t *words, uint32_t n)
{
	if ((addr & 3) || n > STORE_FLASH_SIZE / 4 || !in_store(addr, n * 4))
		return -1;
	uint32_t flags = irq_save();
	int r = ((program_fn)ROM_PROGRAM_MAIN)(PROGRAM_KEY, words, (uint32_t *)(uintptr_t)addr, n);
	irq_restore(flags);
	return r ? -2 : 0;
}
