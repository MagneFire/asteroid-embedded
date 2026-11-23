/* Finding an item in SMEM, no hardware involved. The allocation table
 * near the start holds the items every processor shares. Newer SMEM also
 * has partitions, each shared by two hosts and listed in a table of
 * contents in the last 4 KiB; an item of the pair, past the fixed ones,
 * is in their partition when it exists. A partition's uncached items
 * follow its header, each after a header of its own. */
#include <stddef.h>
#include "smem_find.h"

#define ALLOC_TABLE		0xd0
#define TOC_MAGIC		0x434f5424u	/* "$TOC" */
#define PART_MAGIC		0x54525024u	/* "$PRT" */
#define CANARY			0xa5a5
#define FIXED_ITEM_LAST		8
#define HOST_APPS		0

struct alloc_entry { uint32_t allocated, offset, size, base_ext; };
struct toc_entry { uint32_t offset, size, flags; uint16_t host0, host1; uint32_t size_cacheline, reserved[3], exclusion[4]; };
struct toc { uint32_t magic, version, entries, reserved[5]; struct toc_entry entry[]; };
struct part { uint32_t magic; uint16_t host0, host1; uint32_t size, free_uncached, free_cached, reserved[3]; };
struct part_item { uint16_t canary, type; uint32_t size; uint16_t padding_data, padding_hdr; uint32_t reserved; };

static const volatile struct part *partition(const volatile uint8_t *smem, uint32_t smem_size, unsigned host)
{
	if (smem_size < 4096)
		return NULL;
	const volatile struct toc *t = (const volatile struct toc *)(smem + smem_size - 4096);
	if (t->magic != TOC_MAGIC || t->entries > (4096 - sizeof *t) / sizeof t->entry[0])
		return NULL;
	for (uint32_t i = 0; i < t->entries; i++) {
		const volatile struct toc_entry *e = &t->entry[i];
		bool ours = (e->host0 == HOST_APPS && e->host1 == host) || (e->host1 == HOST_APPS && e->host0 == host);
		if (!ours || !e->offset || !e->size || e->offset >= smem_size || e->size > smem_size - e->offset)
			continue;
		const volatile struct part *p = (const volatile struct part *)(smem + e->offset);
		bool hosts = (p->host0 == HOST_APPS || p->host1 == HOST_APPS) && (p->host0 == host || p->host1 == host);
		if (p->magic == PART_MAGIC && hosts && p->free_uncached <= p->size && p->free_cached <= p->size &&
		    p->size <= e->size)
			return p;
	}
	return NULL;
}

int smem_find(const volatile void *base, uint32_t smem_size, unsigned id, int host, struct smem_loc *loc)
{
	const volatile uint8_t *smem = base;
	const volatile struct part *p = host >= 0 && id > FIXED_ITEM_LAST ? partition(smem, smem_size, (unsigned)host) : NULL;
	if (p) {
		const volatile uint8_t *at = (const volatile uint8_t *)p + sizeof *p, *end = (const volatile uint8_t *)p + p->free_uncached;
		while (at + sizeof(struct part_item) <= end) {
			const volatile struct part_item *it = (const volatile struct part_item *)at;
			if (it->canary != CANARY)
				return -2;
			const volatile uint8_t *data = at + sizeof *it + it->padding_hdr;
			if (data > end || it->size > (uint32_t)(end - data) || it->padding_data > it->size)
				return -2;
			if (it->type == id) {
				*loc = (struct smem_loc){ (uint32_t)(data - smem), (it->size - it->padding_data + 7) & ~7u, 0, true };
				return 0;
			}
			at = data + it->size;
		}
		return -1;
	}
	if (ALLOC_TABLE + (id + 1) * sizeof(struct alloc_entry) > smem_size)
		return -1;
	const volatile struct alloc_entry *e = (const volatile struct alloc_entry *)(smem + ALLOC_TABLE) + id;
	if (!e->allocated || !e->size)
		return -1;
	/* The low two bits of the base are not part of the address. */
	uint32_t base_ext = e->base_ext & ~3u;
	if (!base_ext && (e->offset >= smem_size || e->size > smem_size - e->offset))
		return -2;
	*loc = (struct smem_loc){ e->offset, e->size, base_ext, false };
	return 0;
}
