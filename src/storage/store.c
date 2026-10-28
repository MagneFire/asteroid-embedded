/* Our own data lives in the raw STORE_PARTITION, the only place the eMMC
 * write guard allows. */
#include <string.h>
#include "asteroid.h"
#include "storage.h"

static uint32_t store_gen;
static struct store_info store;

static int resolve_partition(void)
{
	uint32_t first, count;
	if (emmc_part_find(STORE_PARTITION, &first, &count)) {
		store.why = "no " STORE_PARTITION " partition";
		return -45;
	}
	if (count * EMMC_BLOCK < STORE_MIN_SIZE) {
		store.why = "partition too small";
		return -46;
	}
	if (count > STORE_MAX_SIZE / EMMC_BLOCK)
		count = STORE_MAX_SIZE / EMMC_BLOCK;
	store.size = count * EMMC_BLOCK;
	store.segs[0] = (struct store_seg){ 0, first, count };
	store.nsegs = 1;
	store.why = NULL;
	return 0;
}

int store_open(void)
{
	if (store.open && emmc_card()->ready && emmc_card()->generation == store_gen)
		return 0;
	memset(&store, 0, sizeof store);
	int r;
	if (!emmc_card()->ready && (r = emmc_init(true)))
		return r;
	if (!emmc_part_count() && (r = emmc_part_scan()))
		return r;
	r = resolve_partition();
	if (r) {
		store.nsegs = 0;
		log_printf("storage: no store: %s\n", store.why);
		return r;
	}
	store.open = true;
	store_gen = emmc_card()->generation;
	if (!store.why) {
		struct emmc_range g[ARRAY_SIZE(store.segs)];
		for (unsigned i = 0; i < store.nsegs; i++)
			g[i] = (struct emmc_range){ store.segs[i].lba, store.segs[i].count };
		if (emmc_guard_set(g, store.nsegs))
			store.why = "store outside the " EMMC_WRITE_PARTITION " partition";
	}
	store.writable = !store.why;
	log_printf("storage: store %lu bytes in %u runs, %s%s\n", store.size, store.nsegs, store.writable ? "writable" : "read-only: ",
		   store.why ? store.why : "");
	return 0;
}

const struct store_info *store_info(void)
{
	return &store;
}

static int store_io(uint32_t off, void *buf, uint32_t len, bool write)
{
	if (!store.open || !emmc_card()->ready || emmc_card()->generation != store_gen)
		return -1;
	if (write && !store.writable)
		return -2;
	if ((off | len) & (EMMC_BLOCK - 1) || off > store.size || len > store.size - off)
		return -3;
	uint8_t *p = buf;
	for (unsigned i = 0; i < store.nsegs && len; i++) {
		const struct store_seg *s = &store.segs[i];
		uint32_t end = s->off + s->count * EMMC_BLOCK;
		if (off < s->off || off >= end)
			continue;
		uint32_t k = end - off < len ? end - off : len;
		uint32_t lba = s->lba + (off - s->off) / EMMC_BLOCK;
		int r = write ? emmc_write(lba, k / EMMC_BLOCK, p) : emmc_read(lba, k / EMMC_BLOCK, p);
		if (r)
			return r;
		p += k;
		off += k;
		len -= k;
	}
	return len ? -4 : 0;
}

int store_read(uint32_t off, void *buf, uint32_t len)
{
	return store_io(off, buf, len, false);
}

int store_write(uint32_t off, const void *buf, uint32_t len)
{
	return store_io(off, (void *)(uintptr_t)buf, len, true);
}
