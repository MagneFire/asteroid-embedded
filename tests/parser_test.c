/* Host tests of the parsers that take bytes from outside the firmware:
 * the phone's notification, weather and media writes, the key/value
 * slots and ext4 structures read from the eMMC. Built with the address
 * and undefined behaviour sanitizers, each gets known cases and then
 * random or mutated input, so an out-of-bounds access fails the test
 * even where the parsed result looks fine. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asteroid.h"
#include "notify/notify.h"
#include "weather/weather.h"
#include "media/media.h"
#include "storage/storage.h"

static int failures;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint32_t rng = 1;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

uint32_t time_ms(void) { return 0; }
uint32_t pmic_rtc_read(void) { return 1000; }
void log_printf(const char *fmt, ...) {}
void wdt_feed(void) {}
int bt_gatt_media_command(const uint8_t *v, unsigned n) { return 0; }

static uint8_t store[2 * KV_SLOT_SIZE];
static struct store_info info = { .open = true, .writable = true, .size = sizeof store };
static struct emmc_info card;
int store_open(void) { return 0; }
const struct store_info *store_info(void) { return &info; }
int store_read(uint32_t off, void *buf, uint32_t len)
{
	if (off > sizeof store || len > sizeof store - off)
		return -1;
	memcpy(buf, store + off, len);
	return 0;
}
int store_write(uint32_t off, const void *buf, uint32_t len)
{
	if (off > sizeof store || len > sizeof store - off)
		return -1;
	memcpy(store + off, buf, len);
	return 0;
}
const struct emmc_info *emmc_card(void) { return &card; }

static uint8_t *disk;
static uint32_t disk_sectors;
int emmc_read(uint32_t lba, uint32_t n, void *buf)
{
	if (lba > disk_sectors || n > disk_sectors - lba)
		return -1;
	memcpy(buf, disk + (size_t)lba * EMMC_BLOCK, (size_t)n * EMMC_BLOCK);
	return 0;
}

uint32_t crc32(uint32_t crc, const void *buf, uint32_t n)
{
	const uint8_t *p = buf;
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
	}
	return ~crc;
}

static void feed(const char *s)
{
	notify_feed((const uint8_t *)s, (unsigned)strlen(s));
}

static void test_notify(void)
{
	feed("<insert><id>7</id><an>Mail</an><su>Hi &amp; bye</su><bo>Body</bo></insert>");
	CHECK(notify_count() == 1);
	CHECK(!strcmp(notify_get(0)->summary, "Hi & bye"));
	feed("<insert><id>7</id><an>Ma");
	feed("il</an><su>Again</su></insert>");
	CHECK(notify_count() == 1 && !strcmp(notify_get(0)->summary, "Again"));
	feed("<removed><id>7</id></removed>");
	CHECK(notify_count() == 0);

	char big[600];
	memset(big, 'x', sizeof big);
	memcpy(big + sizeof big - 4, "\xd0\x9f\xd1", 3);
	big[sizeof big - 1] = 0;
	char msg[800];
	snprintf(msg, sizeof msg, "<insert><id>-3</id><bo>%s</bo></insert>", big);
	feed(msg);
	CHECK(notify_count() == 1 && strlen(notify_get(0)->body) == sizeof notify_get(0)->body - 1);
	CHECK(notify_get(0)->id == -3);
	notify_dismiss(-3);

	for (int i = 0; i < 25; i++) {
		snprintf(msg, sizeof msg, "<insert><id>%d</id></insert>", i);
		feed(msg);
	}
	CHECK(notify_count() == NOTIFY_MAX);

	static const char *const parts[] = { "<insert>", "</insert>", "<removed>", "</removed>", "<id>", "</id>",
					     "<su>", "</su>", "<bo>", "</bo>", "&amp;", "&", "<", ">", "1", "\xe2\x82",
					     "\xac", "&quot" };
	for (int round = 0; round < 20000; round++) {
		uint8_t p[300];
		unsigned n = rnd() % sizeof p;
		for (unsigned i = 0; i < n;) {
			if (rnd() % 3) {
				const char *s = parts[rnd() % ARRAY_SIZE(parts)];
				unsigned l = (unsigned)strlen(s);
				if (l > n - i)
					l = n - i;
				memcpy(p + i, s, l);
				i += l;
			} else {
				p[i++] = (uint8_t)rnd();
			}
		}
		notify_feed(p, n);
	}
	for (unsigned i = 0; i < notify_count(); i++) {
		const struct notification *n = notify_get(i);
		CHECK(memchr(n->summary, 0, sizeof n->summary) && memchr(n->body, 0, sizeof n->body));
	}
}

static void test_weather_media(void)
{
	weather_write(0x8001, (const uint8_t *)"Usselo", 6);
	CHECK(!strcmp(weather_get()->city, "Usselo"));
	uint8_t ids[] = { 3, 32, 3, 33, 2, 0 };
	weather_write(0x8002, ids, sizeof ids);
	CHECK(weather_get()->days == 3 && weather_get()->id[0] == 800 && weather_get()->id[2] == 512);
	media_write(0x7001, (const uint8_t *)"Title", 5);
	media_write(0x7006, (const uint8_t *)"\xff", 1);
	CHECK(!strcmp(media_get()->title, "Title") && media_get()->volume == 100);
	for (int round = 0; round < 20000; round++) {
		uint8_t p[200];
		unsigned n = rnd() % sizeof p;
		for (unsigned i = 0; i < n; i++)
			p[i] = (uint8_t)rnd();
		weather_write((uint16_t)(0x8000 + rnd() % 6), p, n);
		media_write((uint16_t)(0x7000 + rnd() % 8), p, n);
	}
	CHECK(weather_get()->days <= WEATHER_DAYS && memchr(weather_get()->city, 0, sizeof weather_get()->city));
}

static void test_kv(void)
{
	CHECK(kv_set("a", "1", 1) == 0);
	CHECK(kv_set("weather", "sunny", 5) == 0);
	char v[16] = { 0 };
	CHECK(kv_get("weather", v, sizeof v) == 5 && !memcmp(v, "sunny", 5));
	CHECK(kv_del("a") == 0 && kv_get("a", v, sizeof v) < 0);
	uint8_t val[KV_VALUE_MAX];
	memset(val, 7, sizeof val);
	for (int i = 0; i < 40; i++) {
		char key[KV_KEY_MAX + 1];
		snprintf(key, sizeof key, "key%d", i);
		kv_set(key, val, sizeof val);
	}
	static uint8_t good[sizeof store];
	memcpy(good, store, sizeof store);
	for (int round = 0; round < 3000; round++) {
		memcpy(store, good, sizeof store);
		for (int k = 1 + rnd() % 8; k; k--)
			store[rnd() % sizeof store] = (uint8_t)rnd();
		card.generation++;
		kv_get("weather", v, sizeof v);
		kv_set("x", "y", 1);
	}
}

static void list_fn(const char *name, unsigned len, uint32_t ino, unsigned type, void *ctx)
{
	(*(unsigned *)ctx)++;
}

static void walk_fs(bool expect)
{
	static struct ext4 fs;
	struct ext4_inode in;
	static uint8_t buf[70000];
	int r = ext4_open_lba(&fs, "test", 0, disk_sectors);
	if (expect)
		CHECK(r == 0);
	if (r)
		return;
	r = ext4_lookup(&fs, "/etc/asteroid/machine.conf", &in);
	if (expect)
		CHECK(r == 0 && in.size == 9);
	if (!r && in.size <= sizeof buf) {
		r = ext4_read(&fs, &in, 0, buf, (uint32_t)in.size);
		if (expect)
			CHECK(r == 9 && !memcmp(buf, "sturgeon\n", 9));
	}
	r = ext4_lookup(&fs, "/usr/link", &in);
	if (expect)
		CHECK(r == 0 && in.size == 9);
	if (!ext4_lookup(&fs, "/usr/share/big.txt", &in)) {
		uint32_t n = in.size < sizeof buf ? (uint32_t)in.size : sizeof buf;
		r = ext4_read(&fs, &in, 0, buf, n);
		if (expect)
			CHECK(r == (int)n && n > 20000);
		ext4_read(&fs, &in, in.size / 2 + 1, buf, 1000);
	} else if (expect) {
		CHECK(!"big.txt");
	}
	unsigned entries = 0;
	if (!ext4_lookup(&fs, "/dir", &in))
		ext4_list(&fs, &in, list_fn, &entries);
	if (expect)
		CHECK(entries >= 60);
	ext4_lookup(&fs, "/dir/file-with-a-longer-name-42", &in);
	ext4_lookup(&fs, "/nope/../etc//asteroid/./machine.conf", &in);
	for (uint32_t ino = 1; ino < 40; ino++)
		if (!ext4_read_inode(&fs, ino, &in)) {
			uint64_t pblk, count;
			uint32_t flags;
			ext4_map(&fs, &in, 0, &pblk, &count, &flags);
			ext4_read(&fs, &in, 0, buf, 4096);
		}
}

static void test_ext4(const char *image)
{
	FILE *f = fopen(image, "rb");
	CHECK(f);
	if (!f)
		return;
	static uint8_t good[1 << 20];
	size_t n = fread(good, 1, sizeof good, f);
	fclose(f);
	disk = malloc(n);
	disk_sectors = (uint32_t)(n / EMMC_BLOCK);
	memcpy(disk, good, n);
	walk_fs(true);
	for (int round = 0; round < 4000; round++) {
		memcpy(disk, good, n);
		for (int k = 1 + rnd() % 16; k; k--) {
			uint32_t at = rnd() % 64 < 48 ? rnd() % 16384 : rnd() % (uint32_t)n;
			disk[at] = rnd() % 4 ? (uint8_t)rnd() : (uint8_t)(disk[at] ^ (1u << (rnd() % 8)));
		}
		walk_fs(false);
	}
	free(disk);
}

int main(int argc, char **argv)
{
	test_notify();
	test_weather_media();
	test_kv();
	test_ext4(argc > 1 ? argv[1] : "build/test/small.ext4");
	if (failures)
		return 1;
	printf("parser_test: ok\n");
	return 0;
}
