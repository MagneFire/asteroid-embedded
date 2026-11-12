/* The SGX start-up image against what the vendor's own library builds,
 * as tools/sgx/srvinit_emu.py recorded it. Needs the library from a
 * dump of the watch (SGX_LIBSRV_INIT) and the recording (SGX_REFERENCE,
 * build/sgx/init by default); without them it only checks the tables. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/sgx_image.h"

#define POOL	(4u << 20)
#define PHYS	0x8f000000u

static uint8_t *lib;
static long lib_size;
static int failures;

#define CHECK(cond, ...) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
			failures++; \
		} \
	} while (0)

static uint8_t *slurp(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	rewind(f);
	uint8_t *p = malloc((size_t)*size + 1);
	if (fread(p, 1, (size_t)*size, f) != (size_t)*size)
		exit(2);
	fclose(f);
	return p;
}

static bool read_lib(void *ctx, uint32_t off, void *buf, uint32_t len)
{
	if (off + len > (uint32_t)lib_size)
		return false;
	memcpy(buf, lib + off, len);
	return true;
}

static bool read_zero(void *ctx, uint32_t off, void *buf, uint32_t len)
{
	memset(buf, 0, len);
	return true;
}

static void tables(void)
{
	struct sgx_mem m;
	struct sgx_image img;
	struct sgx_reg r[SGX_SCRIPT_MAX];
	uint8_t *pool = calloc(1, POOL);
	sgx_mem_init(&m, pool, PHYS, POOL);
	CHECK(sgx_image_build(&m, &img, read_zero, NULL), "build");
	CHECK(img.ccb == 0x0f000000 && img.code[0] == 0x0e400000 && img.code[3] == 0x0e418000, "addresses");
	CHECK(sgx_ptr(&m, img.host_ctl) && !sgx_ptr(&m, 0x0a000000), "walk");
	uint32_t *pd = (uint32_t *)(void *)pool;
	CHECK((pd[0x0f000000 >> 22] & 1) && !(pd[0] & 1), "directory");
	CHECK(sgx_script(&img, SGX_SCRIPT_BEFORE_RESET, r) == 2, "first script");
	CHECK(sgx_script(&img, SGX_SCRIPT_AFTER_RESET, r) == 33 && r[32].off == 0xa58, "second script");
	CHECK(sgx_script(&img, SGX_SCRIPT_OFF, r) == 8, "off script");
	CHECK(sgx_crc32(0, "123456789", 9) == 0xcbf43926, "crc");
	/* A pool too small fails cleanly. */
	sgx_mem_init(&m, pool, PHYS, 64 * 1024);
	CHECK(!sgx_image_build(&m, &img, read_zero, NULL), "small pool");
	free(pool);
}

static void reference(const char *dir)
{
	struct sgx_mem m;
	struct sgx_image img;
	char path[512], line[256], name[64], which[16];
	uint8_t *pool = calloc(1, POOL);
	sgx_mem_init(&m, pool, PHYS, POOL);
	CHECK(sgx_crc32(0, lib + SGX_FILE_FIRST, SGX_FILE_END - SGX_FILE_FIRST) == SGX_FILE_CRC, "library is another build");
	CHECK(sgx_image_build(&m, &img, read_lib, NULL), "build");

	uint32_t addrs[21];
	memcpy(addrs, &img.ccb, sizeof addrs);
	struct sgx_reg scripts[3][SGX_SCRIPT_MAX];
	unsigned counts[3], seen[3] = { 0 }, allocs = 0;
	for (unsigned i = 0; i < 3; i++)
		counts[i] = sgx_script(&img, i, scripts[i]);

	snprintf(path, sizeof path, "%s/init.txt", dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		printf("FAIL no %s (run tools/sgx/flatten.py)\n", path);
		failures++;
		return;
	}
	while (fgets(line, sizeof line, f)) {
		unsigned index, heap, size, flags, addr, off, value;
		if (sscanf(line, "alloc %u %u %x %x %x %63s", &index, &heap, &size, &flags, &addr, name) == 6) {
			allocs++;
			CHECK(index < 21 && addrs[index] == addr, "allocation %u at %08x, vendor %08x", index, addrs[index], addr);
			if (name[0] == '-' || index >= 21)
				continue;
			long n;
			snprintf(path, sizeof path, "%s/%s", dir, name);
			uint8_t *want = slurp(path, &n), *got = sgx_ptr(&m, addr);
			CHECK(want && got && n == (long)size, "allocation %u unreadable", index);
			for (long i = 0; want && got && i < n; i++)
				if (want[i] != got[i]) {
					CHECK(0, "allocation %u differs at +0x%lx: %02x, vendor %02x", index, i, got[i], want[i]);
					break;
				}
			free(want);
		} else if (sscanf(line, "script %15s %x %x", which, &off, &value) == 3) {
			unsigned s = !strcmp(which, "before") ? 0 : !strcmp(which, "after") ? 1 : 2, i = seen[s]++;
			CHECK(i < counts[s] && scripts[s][i].off == off && scripts[s][i].value == value,
			      "script %s entry %u: %x = %x, vendor %x = %x", which, i, i < counts[s] ? scripts[s][i].off : 0,
			      i < counts[s] ? scripts[s][i].value : 0, off, value);
		}
	}
	fclose(f);
	CHECK(allocs == 21, "%u allocations in the reference", allocs);
	for (unsigned i = 0; i < 3; i++)
		CHECK(seen[i] == counts[i], "script %u: %u entries, vendor %u", i, counts[i], seen[i]);
	free(pool);
}

int main(void)
{
	tables();
	const char *path = getenv("SGX_LIBSRV_INIT"), *dir = getenv("SGX_REFERENCE");
	if (path && (lib = slurp(path, &lib_size)))
		reference(dir ? dir : "build/sgx/init");
	else
		printf("sgx_image_test: no vendor library, reference not compared\n");
	if (failures)
		return 1;
	printf("sgx_image_test: ok\n");
	return 0;
}
