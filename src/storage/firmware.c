/* Vendor firmware: the GPU microcode, the Bluetooth patch and the like
 * are read from the watch's Android system partition, where its own
 * software keeps them, so none of it is part of this image. The
 * partition is ext4 on some watches and squashfs on others. The board
 * lists the directories to look in. Each file is read once into a fixed
 * pool and kept there. */
#include <string.h>
#include "asteroid.h"
#include "storage.h"
#include "squashfs.h"

#define POOL_SIZE	(128u * 1024)
#define MAX_FILES	6

static struct ext4 fs;
static struct squashfs squash;
static bool opened, squashed;
static uint8_t pool[POOL_SIZE] __attribute__((aligned(64)));
static uint32_t used;
static unsigned nfiles;
static struct { char name[48]; uint32_t off, size; } files[MAX_FILES];

static int open_system(void)
{
	if (opened)
		return 0;
	uint32_t first, count;
	int r;
	if ((!emmc_card()->ready && (r = emmc_init(true))) || (!emmc_part_count() && (r = emmc_part_scan())) ||
	    (r = emmc_part_find("system", &first, &count)))
		return r;
	if ((r = ext4_open_lba(&fs, "system", first, count))) {
		if ((r = squashfs_open_lba(&squash, "system", first, count)))
			return r;
		squashed = true;
	}
	opened = true;
	return 0;
}

/* The last file read stays looked up: a caller reads one file in many
 * pieces, and each lookup reads directory blocks again. */
int firmware_read(const char *path, uint32_t off, void *buf, uint32_t len)
{
	static char last[64];
	static struct squashfs_inode sq;
	static struct ext4_inode in;
	int r = open_system();
	if (r)
		return r;
	if (strcmp(path, last)) {
		last[0] = 0;
		if ((r = squashed ? squashfs_lookup(&squash, path, &sq) : ext4_lookup(&fs, path, &in)))
			return r;
		if (strlen(path) < sizeof last)
			strcpy(last, path);
	}
	return squashed ? squashfs_read(&squash, &sq, off, buf, len) : ext4_read(&fs, &in, off, buf, len);
}

const void *firmware_get(const char *name, uint32_t *size)
{
	for (unsigned i = 0; i < nfiles; i++)
		if (!strcmp(files[i].name, name)) {
			*size = files[i].size;
			return pool + files[i].off;
		}
	char path[64];
	struct ext4_inode in;
	size_t len = strlen(name);
	int r;
	if (nfiles == MAX_FILES || len >= sizeof files[0].name)
		return NULL;
	if ((r = open_system())) {
		log_printf("firmware: system partition: %d\n", r);
		return NULL;
	}
	in.size = 0;
	for (unsigned i = 0; ; i++) {
		const char *dir = board->firmware_dirs[i];
		if (!dir) {
			log_printf("firmware: %s: %d\n", name, r);
			return NULL;
		}
		size_t dlen = strlen(dir);
		memcpy(path, dir, dlen);
		memcpy(path + dlen, name, len + 1);
		if (squashed) {
			if ((r = squashfs_read_file(&squash, path, pool + used, POOL_SIZE - used)) >= 0) {
				in.size = (uint32_t)r;
				break;
			}
		} else if (!(r = ext4_lookup(&fs, path, &in)))
			break;
	}
	if (!squashed &&
	    (in.size > POOL_SIZE - used || ext4_read(&fs, &in, 0, pool + used, (uint32_t)in.size) != (int)in.size)) {
		log_printf("firmware: %s: %lu bytes not read\n", path, (unsigned long)in.size);
		return NULL;
	}
	memcpy(files[nfiles].name, name, len + 1);
	files[nfiles].off = used;
	files[nfiles].size = *size = (uint32_t)in.size;
	used = (used + (uint32_t)in.size + 63) & ~63u;
	return pool + files[nfiles++].off;
}
