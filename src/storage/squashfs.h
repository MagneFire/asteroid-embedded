#ifndef SQUASHFS_H
#define SQUASHFS_H
#include <stdint.h>
#include <stdbool.h>

#define SQUASHFS_MAX_BLOCK	131072u
#define SQUASHFS_META_SIZE	8192u
#define SQUASHFS_S_IFMT		0xf000u
#define SQUASHFS_S_IFREG	0x8000u
#define SQUASHFS_S_IFDIR	0x4000u
#define SQUASHFS_S_IFLNK	0xa000u

struct squashfs_inode {
	uint32_t ino, frag, frag_off, list_block, dir_block;
	uint16_t mode, list_off, dir_off;
	uint64_t size, start, ref;
};

struct squashfs_mcache {
	uint64_t addr, next;
	uint32_t len;
	bool valid;
	uint8_t data[SQUASHFS_META_SIZE];
};

struct squashfs {
	const char *name;
	uint32_t first_lba, sectors;
	uint64_t bytes, bytes_used, root, inode_table, dir_table, frag_table;
	uint32_t bs, inodes, fragments;
	struct squashfs_mcache ic, dc, fc;
	uint64_t blk_addr;
	uint32_t blk_len;
	bool blk_valid;
	uint8_t comp[SQUASHFS_MAX_BLOCK + 1024] __attribute__((aligned(64)));
	uint8_t blk[SQUASHFS_MAX_BLOCK] __attribute__((aligned(64)));
};

typedef void (*squashfs_dir_fn)(const char *name, unsigned len, uint32_t ino, unsigned type, void *ctx);

int squashfs_open_lba(struct squashfs *fs, const char *name, uint32_t first_lba, uint32_t sectors);
int squashfs_lookup(struct squashfs *fs, const char *path, struct squashfs_inode *in);
int squashfs_lookup_nofollow(struct squashfs *fs, const char *path, struct squashfs_inode *in);
int squashfs_read(struct squashfs *fs, const struct squashfs_inode *in, uint64_t off, void *buf, uint32_t len);
int squashfs_read_file(struct squashfs *fs, const char *path, void *buf, uint32_t cap);
int squashfs_readlink(struct squashfs *fs, const struct squashfs_inode *in, char *buf, uint32_t cap);
int squashfs_list(struct squashfs *fs, const struct squashfs_inode *dir, squashfs_dir_fn fn, void *ctx);
const char *squashfs_strerror(int err);
#endif
