/* What the bootloader hands over: r2 points at the device tree it updated,
 * whose /chosen node records where it loaded the boot image's ramdisk
 * (linux,initrd-start and -end). The ramdisk is an archive of named blobs
 * built by tools/pack.py: 'AEBL', a count, then name[40]/offset/size
 * entries. */
#include <string.h>
#include "asteroid.h"

uint32_t boot_dtb __attribute__((section(".data")));
static uint32_t rd_start, rd_end;

#define FDT_MAGIC	0xd00dfeedu
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint32_t prop_value(const uint8_t *v, uint32_t len)
{
	return len == 8 ? be32(v + 4) : len == 4 ? be32(v) : 0;
}

static void read_chosen(void)
{
	const uint8_t *fdt = (const uint8_t *)(uintptr_t)boot_dtb;
	if (!boot_dtb || boot_dtb >= 0x02f00000u || be32(fdt) != FDT_MAGIC)
		return;
	const uint8_t *p = fdt + be32(fdt + 8), *strings = fdt + be32(fdt + 12);
	int depth = 0;
	bool in_chosen = false;
	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *name = (const char *)p;
			depth++;
			if (depth == 2)
				in_chosen = !strcmp(name, "chosen");
			p += (strlen(name) + 4) & ~3u;
		} else if (tok == FDT_END_NODE) {
			if (depth-- == 2)
				in_chosen = false;
		} else if (tok == FDT_PROP) {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			const char *name = (const char *)strings + nameoff;
			if (in_chosen && !strcmp(name, "linux,initrd-start"))
				rd_start = prop_value(p + 8, len);
			else if (in_chosen && !strcmp(name, "linux,initrd-end"))
				rd_end = prop_value(p + 8, len);
			p += 8 + ((len + 3) & ~3u);
		} else if (tok != FDT_NOP) {
			return;
		}
	}
}

void boot_init(void)
{
	read_chosen();
	log_printf("boot: device tree %08lx, ramdisk %08lx-%08lx\n", boot_dtb, rd_start, rd_end);
}

const void *boot_blob(const char *name, uint32_t *size)
{
	const uint8_t *rd = (const uint8_t *)(uintptr_t)rd_start;
	if (!rd_start || rd_end - rd_start < 8 || memcmp(rd, "AEBL", 4))
		return NULL;
	uint32_t count = rd[4] | rd[5] << 8 | rd[6] << 16 | (uint32_t)rd[7] << 24;
	for (uint32_t i = 0; i < count && 8 + (i + 1) * 48 <= rd_end - rd_start; i++) {
		const uint8_t *e = rd + 8 + i * 48;
		uint32_t off, len;
		memcpy(&off, e + 40, 4);
		memcpy(&len, e + 44, 4);
		if (!strncmp((const char *)e, name, 40) && off + len <= rd_end - rd_start) {
			*size = len;
			return rd + off;
		}
	}
	return NULL;
}
