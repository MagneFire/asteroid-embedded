/* As the vendor's loader takes it: a segment is loaded when it is
 * PT_LOAD with a size and not the hash table (type 2 in flags bits
 * 24-26). Every one of them must be relocatable (flags bit 27), as the
 * firmware sits in a region of its own instead of where it was linked;
 * the lowest segment's address is the region's start. */
#include <string.h>
#include "mdt.h"

#define PT_LOAD		1
#define EHDR_SIZE	52
#define PHDR_SIZE	32

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int mdt_parse(const uint8_t *mdt, uint32_t len, uint32_t region_size, struct mdt_image *img)
{
	memset(img, 0, sizeof *img);
	if (len < EHDR_SIZE || memcmp(mdt, "\x7f" "ELF", 4) || mdt[4] != 1 || mdt[5] != 1)
		return -1;
	uint32_t phoff = rd32(mdt + 28);
	uint16_t phentsize = rd16(mdt + 42), phnum = rd16(mdt + 44);
	if (phentsize != PHDR_SIZE || !phnum || phoff > len || phnum > (len - phoff) / PHDR_SIZE)
		return -2;
	uint32_t lo = UINT32_MAX, hi = 0;
	for (unsigned i = 0; i < phnum; i++) {
		const uint8_t *p = mdt + phoff + i * PHDR_SIZE;
		uint32_t type = rd32(p), paddr = rd32(p + 12), filesz = rd32(p + 16), memsz = rd32(p + 20), flags = rd32(p + 24);
		if (type != PT_LOAD || !memsz || ((flags >> 24) & 7) == 2)
			continue;
		if (!(flags & (1u << 27)) || filesz > memsz || paddr > UINT32_MAX - memsz || img->nsegs == MDT_MAX_SEGS)
			return -3;
		img->seg[img->nsegs++] = (struct mdt_seg){ (uint8_t)i, paddr, filesz, memsz };
		if (paddr < lo)
			lo = paddr;
		if (paddr + memsz > hi)
			hi = paddr + memsz;
	}
	if (!img->nsegs)
		return -4;
	hi = (hi + 0xfff) & ~0xfffu;
	if (hi - lo > region_size)
		return -5;
	img->base = lo;
	img->size = hi - lo;
	img->entry = rd32(mdt + 24);
	for (unsigned i = 0; i < img->nsegs; i++)
		img->seg[i].offset -= lo;
	return 0;
}
