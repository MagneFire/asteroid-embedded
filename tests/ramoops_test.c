/* Ramoops zones as swift's AsteroidOS kernel reads them. */
#include <stdio.h>
#include <string.h>
#include "lib/ramoops.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int main(void)
{
	/* swift: 2 MiB, 64 KiB records, 64 KiB console, 4 KiB ftrace: 30
	 * records, then the console. */
	struct ramoops_layout swift = { 0x8fe00000, 0x200000, 0x10000, 0x10000, 0x1000 };
	CHECK(ramoops_console_offset(&swift) == 30 * 0x10000);
	struct ramoops_layout none = { 0, 0x10000, 0x10000, 0x10000, 0 };
	CHECK(ramoops_console_offset(&none) == 0);

	uint8_t zone[12 + 8];
	memset(zone, 0xaa, sizeof zone);
	ramoops_zone_fill(zone, sizeof zone, (const uint8_t *)"abc", 3, (const uint8_t *)"de", 2);
	CHECK(get32(zone) == RAMOOPS_SIG && get32(zone + 4) == 0 && get32(zone + 8) == 5);
	CHECK(!memcmp(zone + 12, "abcde", 5));
	/* Too long: the end of b, after what is left of a. */
	ramoops_zone_fill(zone, sizeof zone, (const uint8_t *)"0123456", 7, (const uint8_t *)"789", 3);
	CHECK(get32(zone + 8) == 8 && !memcmp(zone + 12, "23456789", 8));
	ramoops_zone_fill(zone, sizeof zone, (const uint8_t *)"01", 2, (const uint8_t *)"abcdefghij", 10);
	CHECK(get32(zone + 8) == 8 && !memcmp(zone + 12, "cdefghij", 8));

	if (failures)
		printf("%d failures\n", failures);
	else
		printf("ramoops: all passed\n");
	return failures != 0;
}
