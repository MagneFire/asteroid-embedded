#ifndef EXT4_H
#define EXT4_H
#include <stdint.h>
#include <stdbool.h>

#define EXT4_MAX_BLOCK		4096u
#define EXT4_S_IFMT		0xf000u
#define EXT4_S_IFREG		0x8000u
#define EXT4_S_IFDIR		0x4000u
#define EXT4_S_IFLNK		0xa000u
#define EXT4_INCOMPAT_RECOVER	0x0004u
#define EXT4_FL_JOURNAL_DATA	0x00004000u
#define EXT4_FL_VERITY		0x00100000u

#define EXT4_MAP_HOLE		1u
#define EXT4_MAP_UNWRITTEN	2u

struct ext4_inode {
	uint32_t ino, flags;
	uint16_t mode, links;
	uint64_t size;
	uint8_t iblock[60];
};

struct ext4 {
	const char *name;
	struct ext4 *parent;
	struct ext4_inode backing;
	uint32_t first_lba, sectors;
	uint64_t bytes, blocks;
	uint32_t bs, inodes, inodes_per_group, inode_size, desc_size, first_data_block;
	uint32_t compat, incompat, ro_compat, default_mount_opts;
	uint16_t state;
	char label[17];
	struct { uint64_t lblk, pblk, count; uint32_t flags; bool valid; } cache;
	uint8_t node[EXT4_MAX_BLOCK] __attribute__((aligned(64)));
	uint8_t blk[EXT4_MAX_BLOCK] __attribute__((aligned(64)));
};

typedef int (*ext4_seg_fn)(uint32_t lba, uint32_t sectors, uint32_t buf_off, void *ctx);
typedef void (*ext4_dir_fn)(const char *name, unsigned len, uint32_t ino, unsigned type, void *ctx);

int ext4_open_lba(struct ext4 *fs, const char *name, uint32_t first_lba, uint32_t sectors);
int ext4_open_file(struct ext4 *fs, const char *name, struct ext4 *parent, const struct ext4_inode *file);
int ext4_read_inode(struct ext4 *fs, uint32_t ino, struct ext4_inode *in);
int ext4_lookup(struct ext4 *fs, const char *path, struct ext4_inode *in);
int ext4_map(struct ext4 *fs, const struct ext4_inode *in, uint64_t lblk, uint64_t *pblk, uint64_t *count, uint32_t *flags);
int ext4_read(struct ext4 *fs, const struct ext4_inode *in, uint64_t off, void *buf, uint32_t len);
int ext4_list(struct ext4 *fs, const struct ext4_inode *dir, ext4_dir_fn fn, void *ctx);
int ext4_walk(struct ext4 *fs, uint64_t off, uint64_t len, bool strict, ext4_seg_fn fn, void *ctx);
const char *ext4_strerror(int err);
#endif
