/* Vendor firmware: the GPU microcode and the Bluetooth patch are read
 * from /vendor/firmware on the watch's Android system partition, where
 * its own software keeps them, so none of it is part of this image.
 * Each file is read once into a fixed pool and kept there. */
#include <string.h>
#include "asteroid.h"
#include "storage.h"

#define POOL_SIZE	(64u * 1024)
#define MAX_FILES	4

static struct ext4 fs;
static bool opened;
static uint8_t pool[POOL_SIZE] __attribute__((aligned(64)));
static uint32_t used;
static unsigned nfiles;
static struct { char name[24]; uint32_t off, size; } files[MAX_FILES];

static int open_system(void)
{
	if (opened)
		return 0;
	uint32_t first, count;
	int r;
	if ((!emmc_card()->ready && (r = emmc_init(true))) || (!emmc_part_count() && (r = emmc_part_scan())) ||
	    (r = emmc_part_find("system", &first, &count)) || (r = ext4_open_lba(&fs, "system", first, count)))
		return r;
	opened = true;
	return 0;
}

const void *firmware_get(const char *name, uint32_t *size)
{
	for (unsigned i = 0; i < nfiles; i++)
		if (!strcmp(files[i].name, name)) {
			*size = files[i].size;
			return pool + files[i].off;
		}
	char path[64] = "/vendor/firmware/";
	struct ext4_inode in;
	size_t len = strlen(name);
	int r;
	if (nfiles == MAX_FILES || len >= sizeof files[0].name)
		return NULL;
	memcpy(path + 17, name, len + 1);
	if ((r = open_system()) || (r = ext4_lookup(&fs, path, &in))) {
		log_printf("firmware: %s: %d\n", path, r);
		return NULL;
	}
	if (in.size > POOL_SIZE - used || ext4_read(&fs, &in, 0, pool + used, (uint32_t)in.size) != (int)in.size) {
		log_printf("firmware: %s: %lu bytes not read\n", path, (unsigned long)in.size);
		return NULL;
	}
	memcpy(files[nfiles].name, name, len + 1);
	files[nfiles].off = used;
	files[nfiles].size = *size = (uint32_t)in.size;
	used = (used + (uint32_t)in.size + 63) & ~63u;
	return pool + files[nfiles++].off;
}
