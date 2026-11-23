/* SMEM item lookup, in the allocation table and in a partition. */
#include <stdio.h>
#include <string.h>
#include "soc/qcom/smem_find.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define SIZE	0x10000
static uint32_t smem[SIZE / 4];

static void put32(uint32_t off, uint32_t v) { memcpy((uint8_t *)smem + off, &v, 4); }
static void put16(uint32_t off, uint16_t v) { memcpy((uint8_t *)smem + off, &v, 2); }
static uint32_t get32(uint32_t off) { uint32_t v; memcpy(&v, (uint8_t *)smem + off, 4); return v; }

static void alloc(unsigned id, uint32_t offset, uint32_t size, uint32_t base_ext)
{
	uint32_t e = 0xd0 + id * 16;
	put32(e, 1);
	put32(e + 4, offset);
	put32(e + 8, size);
	put32(e + 12, base_ext);
}

/* A partition at 0x8000 for apps and host 4, its items one after the other. */
#define PART	0x8000
static uint32_t part_end = PART + 32;

static void part_item(uint16_t type, uint32_t size, uint16_t padding_data, uint16_t padding_hdr)
{
	put16(part_end, 0xa5a5);
	put16(part_end + 2, type);
	put32(part_end + 4, size);
	put16(part_end + 8, padding_data);
	put16(part_end + 10, padding_hdr);
	part_end += 16 + padding_hdr + size;
	put32(PART + 12, part_end - PART);
}

int main(void)
{
	struct smem_loc l;

	alloc(13, 0x1000, 0x200, 0);
	alloc(137, 0x2000, 0x80, 0);
	alloc(14, 0x40, 0x100, 0x60000);
	CHECK(!smem_find(smem, SIZE, 13, SMEM_HOST_ANY, &l) && l.offset == 0x1000 && l.size == 0x200 && !l.base_ext);
	CHECK(!smem_find(smem, SIZE, 14, SMEM_HOST_ANY, &l) && l.offset == 0x40 && l.base_ext == 0x60000);
	/* The low two bits of the base are flags. */
	alloc(15, 0x80, 0x10, 0x60003);
	CHECK(!smem_find(smem, SIZE, 15, SMEM_HOST_ANY, &l) && l.base_ext == 0x60000);
	alloc(15, 0x80, 0x10, 0x2);
	CHECK(!smem_find(smem, SIZE, 15, SMEM_HOST_ANY, &l) && !l.base_ext);
	CHECK(smem_find(smem, SIZE, 20, SMEM_HOST_ANY, &l) == -1);
	/* An item running off the end. */
	alloc(21, SIZE - 0x10, 0x20, 0);
	CHECK(smem_find(smem, SIZE, 21, SMEM_HOST_ANY, &l) == -2);
	/* No table of contents: a host's items are the shared ones. */
	CHECK(!smem_find(smem, SIZE, 13, SMEM_HOST_WCNSS, &l) && l.offset == 0x1000 && !l.partitioned);

	uint32_t toc = SIZE - 4096;
	put32(toc, 0x434f5424);
	put32(toc + 8, 2);
	/* The first entry is the modem's, the second ours. */
	put32(toc + 32, 0x4000);
	put32(toc + 36, 0x1000);
	put16(toc + 44, 0);
	put16(toc + 46, 1);
	put32(toc + 48, 32);
	uint32_t e = toc + 32 + 48;
	put32(e, PART);
	put32(e + 4, 0x2000);
	put16(e + 12, 4);
	put16(e + 14, 0);
	put32(e + 16, 32);
	put32(PART, 0x54525024);
	put16(PART + 4, 0);
	put16(PART + 6, 4);
	put32(PART + 8, 0x2000);
	part_item(451, 0x40, 4, 0);
	part_item(14 + 3, 0x24, 0, 8);
	CHECK(!smem_find(smem, SIZE, 451, SMEM_HOST_WCNSS, &l) && l.partitioned && l.offset == PART + 32 + 16 && l.size == 0x40);
	CHECK(!smem_find(smem, SIZE, 17, SMEM_HOST_WCNSS, &l) && l.offset == PART + 32 + 16 + 0x40 + 16 + 8 && l.size == 0x28);
	/* In a partition, not in the table, and the other way round. */
	CHECK(smem_find(smem, SIZE, 451, SMEM_HOST_ANY, &l) == -1);
	CHECK(smem_find(smem, SIZE, 137, SMEM_HOST_WCNSS, &l) == -1);
	/* The fixed items are always shared. */
	alloc(3, 0x3000, 0x10, 0);
	CHECK(!smem_find(smem, SIZE, 3, SMEM_HOST_WCNSS, &l) && l.offset == 0x3000);
	/* Added after the last item, the space taken from the free part. */
	put32(PART + 16, 0x2000);
	uint32_t at = part_end;
	CHECK(!smem_alloc(smem, SIZE, 431, SMEM_HOST_WCNSS, 340, &l) && l.partitioned && l.offset == at + 16 && l.size == 344);
	CHECK(get32(at) == (431u << 16 | 0xa5a5) && get32(at + 4) == 344 && get32(at + 8) == 4);
	CHECK(get32(PART + 12) == at + 16 + 344 - PART);
	CHECK(!smem_find(smem, SIZE, 431, SMEM_HOST_WCNSS, &l) && l.offset == at + 16 && l.size == 344);
	/* Found the second time, nothing more taken. */
	CHECK(!smem_alloc(smem, SIZE, 431, SMEM_HOST_WCNSS, 340, &l) && l.offset == at + 16);
	CHECK(get32(PART + 12) == at + 16 + 344 - PART);
	/* No room, no partition, a fixed item. */
	put32(PART + 16, get32(PART + 12) + 100);
	CHECK(smem_alloc(smem, SIZE, 432, SMEM_HOST_WCNSS, 340, &l) == -1);
	CHECK(smem_alloc(smem, SIZE, 432, 1, 8, &l) == -1);
	put32(PART + 16, 0x2000);
	CHECK(smem_alloc(smem, SIZE, 7, SMEM_HOST_WCNSS, 8, &l) == -1);
	CHECK(get32(PART + 12) == at + 16 + 344 - PART);
	/* A partition header for other hosts than its entry says is not used. */
	put16(PART + 6, 1);
	CHECK(smem_find(smem, SIZE, 451, SMEM_HOST_WCNSS, &l) == -1);
	put16(PART + 6, 4);
	/* More data padding than data. */
	put16(PART + 32 + 8, 0x41);
	CHECK(smem_find(smem, SIZE, 451, SMEM_HOST_WCNSS, &l) == -2);
	put16(PART + 32 + 8, 4);
	/* A broken canary. */
	put16(PART + 32, 0x1234);
	CHECK(smem_find(smem, SIZE, 17, SMEM_HOST_WCNSS, &l) == -2);

	if (failures)
		printf("%d failures\n", failures);
	else
		printf("smem: all passed\n");
	return failures != 0;
}
