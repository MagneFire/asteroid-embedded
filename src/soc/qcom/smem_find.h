#ifndef SMEM_FIND_H
#define SMEM_FIND_H
#include <stdbool.h>
#include <stdint.h>

/* SMEM's hosts, as its partitions name them. */
#define SMEM_HOST_ANY		(-1)
#define SMEM_HOST_WCNSS		4

/* Where an item is: its offset from SMEM, or with base_ext from that
 * physical address, and whether it is in a partition. */
struct smem_loc {
	uint32_t offset, size, base_ext;
	bool partitioned;
};

/* 0 when found, -1 when not there, -2 when SMEM looks broken. */
int smem_find(const volatile void *smem, uint32_t smem_size, unsigned id, int host, struct smem_loc *loc);
/* An item of the partition with host, found or else added at the end of
 * its uncached items, as the vendor adds one; the caller holds SMEM's
 * allocation lock. 0 when there, -1 when there is no room or no
 * partition, -2 when SMEM looks broken. */
int smem_alloc(volatile void *smem, uint32_t smem_size, unsigned id, unsigned host, uint32_t size, struct smem_loc *loc);
#endif
