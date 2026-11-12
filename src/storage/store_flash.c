/* The store in the chip's own flash, for a watch without an eMMC: one
 * page per key/value slot. A write starts at a page and erases it first;
 * the flash reads like memory. */
#include <string.h>
#include "asteroid.h"
#include "storage.h"

_Static_assert(KV_SLOT_SIZE == FLASH_PAGE && 2 * KV_SLOT_SIZE <= STORE_FLASH_SIZE, "a slot is a flash page");

static struct store_info store;
static const struct emmc_info card = { .ready = true };

/* The kv store follows the eMMC's generation to notice a new card; this
 * one never changes. */
const struct emmc_info *emmc_card(void) { return &card; }

int store_open(void)
{
	store.open = store.writable = true;
	store.size = STORE_FLASH_SIZE;
	store.nsegs = 0;
	return 0;
}

const struct store_info *store_info(void) { return &store; }

int store_read(uint32_t off, void *buf, uint32_t len)
{
	if (off > STORE_FLASH_SIZE || len > STORE_FLASH_SIZE - off)
		return -3;
	memcpy(buf, (const void *)(uintptr_t)(STORE_FLASH_BASE + off), len);
	return 0;
}

int store_write(uint32_t off, const void *buf, uint32_t len)
{
	if ((off & (FLASH_PAGE - 1)) || (len & 3) || ((uintptr_t)buf & 3) || off > STORE_FLASH_SIZE ||
	    len > STORE_FLASH_SIZE - off)
		return -3;
	for (uint32_t done = 0; done < len || done == 0;) {
		uint32_t k = len - done < FLASH_PAGE ? len - done : FLASH_PAGE;
		int r = flash_erase_page(STORE_FLASH_BASE + off + done);
		if (!r && k)
			r = flash_program(STORE_FLASH_BASE + off + done, (const uint32_t *)((const uint8_t *)buf + done), k / 4);
		if (r)
			return r;
		done += FLASH_PAGE;
		if (!len)
			break;
	}
	return 0;
}
