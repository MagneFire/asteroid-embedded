/* The AsteroidOS root filesystem, found the way the AsteroidOS initramfs
 * finds it on userdata: a loop image at /asteroidos.ext4 or
 * <media>/asteroidos.ext4 (<media> = /media/0 if it exists, else /media),
 * otherwise userdata itself. It is only ever read. */
#include <string.h>
#include "asteroid.h"
#include "storage.h"

static struct ext4 userdata, loopfs;
static struct ext4 *root;
static char where[64];
static bool mounted;
static uint32_t mounted_gen;

static bool exists(struct ext4 *fs, const char *path, uint16_t type, struct ext4_inode *in)
{
	return !ext4_lookup(fs, path, in) && (in->mode & EXT4_S_IFMT) == type;
}

int storage_mount(void)
{
	if (mounted && emmc_card()->ready && emmc_card()->generation == mounted_gen)
		return 0;
	mounted = false;
	int r = 0;
	if (!emmc_card()->ready && (r = emmc_init(true)))
		return r;
	if (!emmc_part_count() && (r = emmc_part_scan()))
		return r;
	uint32_t first, count;
	if (emmc_part_find("userdata", &first, &count))
		return -30;
	if ((r = ext4_open_lba(&userdata, "userdata", first, count))) {
		log_printf("storage: userdata: %s\n", ext4_strerror(r));
		return r;
	}
	struct ext4_inode in;
	const char *media = exists(&userdata, "/media/0", EXT4_S_IFDIR, &in) ? "/media/0" : "/media";
	char path[48];
	snprintf(path, sizeof path, "%s/asteroidos.ext4", media);
	const char *p = exists(&userdata, "/asteroidos.ext4", EXT4_S_IFREG, &in) ? "/asteroidos.ext4"
			: exists(&userdata, path, EXT4_S_IFREG, &in)		 ? path
										 : NULL;
	if (p) {
		if ((r = ext4_open_file(&loopfs, "rootfs", &userdata, &in))) {
			log_printf("storage: %s: %s\n", p, ext4_strerror(r));
			return r;
		}
		root = &loopfs;
		snprintf(where, sizeof where, "userdata:%s", p);
	} else {
		root = &userdata;
		snprintf(where, sizeof where, "userdata");
	}
	mounted = true;
	mounted_gen = emmc_card()->generation;
	log_printf("storage: AsteroidOS root filesystem on %s\n", where);
	return 0;
}

struct ext4 *storage_userdata(void) { return mounted ? &userdata : NULL; }
struct ext4 *storage_rootfs(void) { return mounted ? root : NULL; }
const char *storage_rootfs_where(void) { return mounted ? where : "not mounted"; }

struct ext4 *storage_resolve(const char *path, const char **rest)
{
	if (storage_mount())
		return NULL;
	if (!strncmp(path, "userdata:", 9)) {
		*rest = path[9] ? path + 9 : "/";
		return &userdata;
	}
	*rest = *path ? path : "/";
	return root;
}
