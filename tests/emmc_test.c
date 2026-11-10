/* The eMMC card code on the host, on a host controller that plays a
 * card: the init sequence gets to a ready card, and a disk with the
 * old partition table, laid out as minnow's, gives the partitions the
 * board names. An image of a real disk can be given to scan instead. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asteroid.h"
#include "storage/emmc.h"
#include "storage/emmc_host.h"

#define SECTORS 7733248u
#define KEPT	64

/* The names minnow's board table gives its partitions. */
static const char *const names[] = {
	"", "mbm", "", "ebr", "cdt", "pds", "utags", "logo", "boot", "recovery", "misc", "", "", "system", "cache", "customize",
	"userdata",
};
static const struct board disk_board = { .part_names = names, .part_count = ARRAY_SIZE(names) };
const struct board *board = &disk_board;
bool board_assumed;
uint32_t time_ms(void) { static uint32_t t; return t++; }
void delay_ms(uint32_t ms) {}
void wdt_feed(void) {}
void log_printf(const char *fmt, ...) {}

static FILE *image;
static uint8_t ext_csd[512];
/* The sectors the made-up disk has anything in. */
static struct { uint32_t lba; uint8_t data[512]; } kept[KEPT];
static unsigned nkept;
static unsigned width, open_drain_calls;

static uint8_t *sector(uint32_t lba, bool make)
{
	for (unsigned i = 0; i < nkept; i++)
		if (kept[i].lba == lba)
			return kept[i].data;
	if (!make || nkept == KEPT)
		return NULL;
	kept[nkept].lba = lba;
	return kept[nkept++].data;
}

static void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static void entry(uint8_t *sec, unsigned i, uint8_t type, uint32_t first, uint32_t count)
{
	uint8_t *e = sec + 446 + 16 * i;
	e[4] = type;
	put32(e + 8, first);
	put32(e + 12, count);
	sec[510] = 0x55;
	sec[511] = 0xaa;
}

int emmc_host_init(struct emmc_info *card)
{
	card->clock_hz = 400000;
	memset(ext_csd, 0, sizeof ext_csd);
	put32(ext_csd + 212, SECTORS);
	return 0;
}

int emmc_host_send(struct emmc_cmd *c)
{
	memset(c->resp, 0, sizeof c->resp);
	switch (c->index) {
	case 1:
		c->resp[0] = 0xc0ff8080;
		break;
	case 6:
		if (c->arg >> 24 == 3)
			ext_csd[c->arg >> 16 & 0xff] = (uint8_t)(c->arg >> 8);
		c->resp[0] = 0x900;
		break;
	case 8:
		memcpy(emmc_bounce, ext_csd, 512);
		c->resp[0] = 0x900;
		break;
	case 17:
	case 18:
		for (uint32_t i = 0; i < c->blocks; i++) {
			uint8_t *to = emmc_bounce + i * 512, *from = sector(c->arg + i, false);
			memset(to, 0, 512);
			if (image) {
				fseeko(image, (off_t)(c->arg + i) * 512, SEEK_SET);
				if (fread(to, 1, 512, image) != 512)
					return -1;
			} else if (from)
				memcpy(to, from, 512);
		}
		c->resp[0] = 0x900;
		break;
	default:
		c->resp[0] = 0x900;
	}
	return 0;
}

int emmc_host_clock(uint32_t hz, uint32_t *actual) { *actual = hz; return 0; }
void emmc_host_open_drain(bool on) { open_drain_calls++; }
void emmc_host_high_speed(void) {}
unsigned emmc_host_max_width(void) { return 8; }
void emmc_host_set_width(unsigned bits) { width = bits; }
int emmc_host_rest(bool on) { return 0; }

static void check(bool ok, const char *what)
{
	if (!ok) {
		printf("emmc_test: %s\n", what);
		exit(1);
	}
}

static void expect(const char *name, uint32_t first, uint32_t count)
{
	uint32_t f, c;
	check(!emmc_part_find(name, &f, &c), name);
	check(f == first && c == count, name);
}

/* Three primaries and an extended partition from 3072 with its links
 * in the sectors after that, each naming the next by its number. */
static void minnow_disk(void)
{
	static const uint32_t part[][2] = {
		{ 4096, 1024 }, { 5120, 8192 }, { 13312, 3072 }, { 16384, 2048 }, { 18432, 32768 }, { 51200, 32768 },
		{ 83968, 1024 }, { 84992, 1024 }, { 86016, 16384 }, { 102400, 524288 }, { 626688, 524288 },
		{ 1150976, 262144 }, { 1413120, 6320128 },
	};
	uint8_t *mbr = sector(0, true);
	entry(mbr, 0, 0x83, 256, 256);
	entry(mbr, 1, 0x83, 1024, 1024);
	entry(mbr, 2, 0x83, 2048, 1024);
	entry(mbr, 3, 0x05, 3072, 7730176);
	for (unsigned k = 0; k < ARRAY_SIZE(part); k++) {
		uint8_t *ebr = sector(3072 + k, true);
		entry(ebr, 0, 0x83, part[k][0] - (3072 + k), part[k][1]);
		if (k + 1 < ARRAY_SIZE(part))
			entry(ebr, 1, 0x05, k + 1, 1);
	}
}

int main(int argc, char **argv)
{
	if (argc > 1)
		check((image = fopen(argv[1], "rb")), "the image does not open");
	else
		minnow_disk();
	check(!emmc_init(true), "init failed");
	const struct emmc_info *card = emmc_card();
	check(card->ready && card->sectors == SECTORS && card->bus_width == 8 && width == 8 && card->clock_hz == 50000000,
	      "the card is not as set up");
	check(open_drain_calls == 2, "open drain not switched on and off");
	check(!emmc_part_scan(), "no partition table");
	check(emmc_part_count() == 16, "not 16 partitions");
	expect("p1", 256, 256);
	expect("mbm", 1024, 1024);
	expect("boot", 18432, 32768);
	expect("system", 102400, 524288);
	expect("cache", 626688, 524288);
	expect("userdata", 1413120, 6320128);
	uint32_t f, c;
	check(emmc_part_find("ebr", &f, &c), "the extended partition is listed");
	if (!image) {
		/* A link that points back, and one past the end, end the scan. */
		entry(sector(3072 + 5, false), 1, 0x05, 2, 1);
		check(emmc_part_scan() && !emmc_part_count(), "a loop in the chain was followed");
		entry(sector(3072 + 5, false), 1, 0x05, 7730176, 1);
		check(emmc_part_scan() && !emmc_part_count(), "a link outside the extended partition was followed");
		entry(sector(3072 + 5, false), 1, 0, 0, 0);
		entry(sector(3072 + 5, false), 0, 0x83, 0xfffff800u, 1024);
		check(emmc_part_scan() && !emmc_part_count(), "a partition outside the extended partition was taken");
	}
	printf("emmc_test: ok\n");
	return 0;
}
