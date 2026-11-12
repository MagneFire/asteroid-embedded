/* A watch with no eMMC: no partitions and no file systems to read, so the
 * console's commands for them say so. */
#include "asteroid.h"
#include "storage.h"

int emmc_init(bool fast) { return -1; }
int emmc_read(uint32_t lba, uint32_t n, void *buf) { return -1; }
uint32_t emmc_next_ms(void) { return UINT32_MAX; }
int emmc_part_scan(void) { return -1; }
unsigned emmc_part_count(void) { return 0; }
const struct emmc_part *emmc_part_get(unsigned i) { return NULL; }
int emmc_part_find(const char *name, uint32_t *first, uint32_t *count) { return -1; }

int ext4_lookup(struct ext4 *fs, const char *path, struct ext4_inode *in) { return -1; }
int ext4_read(struct ext4 *fs, const struct ext4_inode *in, uint64_t off, void *buf, uint32_t len) { return -1; }
int ext4_list(struct ext4 *fs, const struct ext4_inode *dir, ext4_dir_fn fn, void *ctx) { return -1; }
const char *ext4_strerror(int err) { return "no eMMC"; }

int storage_mount(void) { return -1; }
struct ext4 *storage_userdata(void) { return NULL; }
struct ext4 *storage_rootfs(void) { return NULL; }
const char *storage_rootfs_where(void) { return "nowhere"; }
struct ext4 *storage_resolve(const char *path, const char **rest) { return NULL; }
int storage_read_file(const char *part, const char *path, void *buf, uint32_t len) { return -1; }
