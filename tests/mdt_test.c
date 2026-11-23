/* Split image headers, with swift's wcnss.mdt program headers. */
#include <stdio.h>
#include <string.h>
#include "soc/qcom/mdt.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* type, offset, paddr, filesz, memsz, flags */
static const uint32_t wcnss[13][6] = {
	{ 0, 0, 0, 0x1d4, 0, 0x07000000 },
	{ 0, 0x1000, 0x8d9ec000, 0x1ac8, 0x2000, 0x02200000 },
	{ 1, 0x3000, 0x8d500000, 0x338c, 0x34c0, 0x88000007 },
	{ 1, 0xafcc, 0x8d504000, 0, 0x8000, 0x08000006 },
	{ 1, 0xafcc, 0x8d50c000, 0xa800, 0xa800, 0x08000006 },
	{ 1, 0x157cc, 0x8d516800, 0, 0xe000, 0x08000006 },
	{ 1, 0x157cc, 0x8d524800, 0x28b9ac, 0x3ef640, 0x08000007 },
	{ 1, 0x2a1178, 0x8d913e80, 0, 0x1c980, 0x08000006 },
	{ 1, 0x2a1178, 0x8d913e80, 0, 0x21c44, 0x08000006 },
	{ 1, 0x2a1178, 0x8d936000, 0x9e4c, 0xdae0, 0x08000007 },
	{ 1, 0x2aafc4, 0x8d950000, 0x38, 0x1b8, 0x08000006 },
	{ 1, 0x2abfcc, 0x8d951000, 0x8c000, 0x8c000, 0x08000006 },
	{ 1, 0x337fcc, 0x8d9dd000, 0xeb8c, 0xeb8c, 0x08000006 },
};

static uint8_t mdt[52 + 13 * 32];

static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void build(unsigned n)
{
	memset(mdt, 0, sizeof mdt);
	memcpy(mdt, "\x7f" "ELF\x01\x01\x01", 7);
	w32(mdt + 24, 0x8d5018f4);
	w32(mdt + 28, 52);
	mdt[42] = 32;
	mdt[44] = (uint8_t)n;
	for (unsigned i = 0; i < n; i++) {
		uint8_t *p = mdt + 52 + i * 32;
		w32(p, wcnss[i][0]);
		w32(p + 4, wcnss[i][1]);
		w32(p + 8, wcnss[i][2]);
		w32(p + 12, wcnss[i][2]);
		w32(p + 16, wcnss[i][3]);
		w32(p + 20, wcnss[i][4]);
		w32(p + 24, wcnss[i][5]);
	}
}

int main(void)
{
	struct mdt_image img;

	/* The real one: loaded from 0x8a600000 to 0x8aaec000 on the watch. */
	build(13);
	CHECK(!mdt_parse(mdt, sizeof mdt, 0x500000, &img));
	CHECK(img.base == 0x8d500000 && img.size == 0x4ec000 && img.entry == 0x8d5018f4);
	CHECK(img.nsegs == 11 && img.seg[0].num == 2 && img.seg[0].offset == 0 && img.seg[0].filesz == 0x338c);
	CHECK(img.seg[1].num == 3 && img.seg[1].filesz == 0 && img.seg[1].memsz == 0x8000 && img.seg[1].offset == 0x4000);
	CHECK(img.seg[10].num == 12 && img.seg[10].offset == 0x4dd000 && img.seg[10].filesz == 0xeb8c);

	/* Too big for the region. */
	CHECK(mdt_parse(mdt, sizeof mdt, 0x400000, &img) == -5);
	/* Program headers past the end of the file. */
	CHECK(mdt_parse(mdt, sizeof mdt - 1, 0x500000, &img) == -2);
	/* Not ELF. */
	mdt[1] = 'F';
	CHECK(mdt_parse(mdt, sizeof mdt, 0x500000, &img) == -1);
	/* A segment that is not relocatable. */
	build(13);
	w32(mdt + 52 + 6 * 32 + 24, 0x00000007);
	CHECK(mdt_parse(mdt, sizeof mdt, 0x500000, &img) == -3);
	/* Only the headers and the hash table. */
	build(2);
	CHECK(mdt_parse(mdt, 52 + 2 * 32, 0x500000, &img) == -4);

	if (failures)
		printf("%d failures\n", failures);
	else
		printf("mdt: all passed\n");
	return failures != 0;
}
