/* A split peripheral image: the .mdt file holds the ELF and program
 * headers and the hash table, each loadable segment N its own .bNN
 * file. No hardware involved. */
#ifndef MDT_H
#define MDT_H
#include <stdint.h>

#define MDT_MAX_SEGS	16

struct mdt_seg {
	uint8_t num;		/* the N of its .bNN file */
	uint32_t offset;	/* from the start of the region */
	uint32_t filesz, memsz;
};

struct mdt_image {
	uint32_t base;		/* the lowest segment's address in the image */
	uint32_t size;		/* up to the 4 KiB after the highest segment */
	uint32_t entry;
	unsigned nsegs;
	struct mdt_seg seg[MDT_MAX_SEGS];
};

/* 0, or < 0 for headers that are not a relocatable image of 32-bit ELF
 * that fits in region_size. */
int mdt_parse(const uint8_t *mdt, uint32_t len, uint32_t region_size, struct mdt_image *img);
#endif
