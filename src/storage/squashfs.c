/* Read-only SquashFS 4.0 with zlib compression, enough for an Android
 * system partition. Everything lives in two kinds of block: metadata
 * blocks (up to 8 KiB uncompressed behind a 16-bit header whose top bit
 * means "stored") holding the inode, directory and fragment tables, and
 * data blocks whose size words sit in the file's inode. Positions inside
 * a table are a (block start, offset in the uncompressed block) pair, so
 * a read may continue into the next block. Three one-block caches keep
 * inode, directory and fragment-table reads from decompressing the same
 * block again and again; the last data or fragment block stays decoded
 * so a run of small reads from one fragment costs one decompression.
 * Directories are scanned linearly, which makes the index of the large
 * directory type unnecessary. Symlinks are followed while a path is
 * resolved, with ".." tracked on a small stack of directory references.
 * Extended attributes, the id table and the export table are ignored.
 * Every size, offset and count read from the image is checked against the
 * volume before use, so a damaged image fails with an error. Block sizes
 * above SQUASHFS_MAX_BLOCK are refused, and so are images over 4 GiB of
 * tables. Nothing here writes. */
#include <string.h>
#include "emmc.h"
#include "squashfs.h"
#include "lib/inflate.h"

#define SB_SIZE			96u
#define SQUASHFS_MAGIC		0x73717368u
#define COMP_ZLIB		1u
#define NO_FRAGMENT		0xffffffffu
#define FRAGS_PER_BLOCK		512u
#define UNCOMPRESSED_BLOCK	(1u << 24)
#define MAX_PATH		256u
#define MAX_HOPS		8u
#define MAX_DEPTH		16u

enum {
	SQE_IO = -200, SQE_RANGE, SQE_MAGIC, SQE_VERSION, SQE_COMPRESSOR, SQE_BLOCKSIZE, SQE_META, SQE_INODE,
	SQE_DIRENT, SQE_NOTDIR, SQE_NOTREG, SQE_NOENT, SQE_LOOP, SQE_DEPTH, SQE_NAMETOOLONG, SQE_SYMLINK,
	SQE_BLOCK, SQE_INFLATE, SQE_FRAGMENT, SQE_TOOBIG, SQE_SHORT,
};

struct mpos {
	uint64_t block;
	uint32_t off;
};

const char *squashfs_strerror(int err)
{
	switch (err) {
	case SQE_IO: return "eMMC read failed";
	case SQE_RANGE: return "outside the volume";
	case SQE_MAGIC: return "no squashfs superblock";
	case SQE_VERSION: return "unsupported squashfs version";
	case SQE_COMPRESSOR: return "compression is not zlib";
	case SQE_BLOCKSIZE: return "unsupported block size";
	case SQE_META: return "corrupt metadata block";
	case SQE_INODE: return "corrupt inode";
	case SQE_DIRENT: return "corrupt directory";
	case SQE_NOTDIR: return "not a directory";
	case SQE_NOTREG: return "not a regular file";
	case SQE_NOENT: return "no such file or directory";
	case SQE_LOOP: return "too many symlinks";
	case SQE_DEPTH: return "path too deep";
	case SQE_NAMETOOLONG: return "path too long";
	case SQE_SYMLINK: return "bad symlink";
	case SQE_BLOCK: return "corrupt data block";
	case SQE_INFLATE: return "decompression failed";
	case SQE_FRAGMENT: return "corrupt fragment";
	case SQE_TOOBIG: return "file larger than the buffer";
	case SQE_SHORT: return "short read";
	default: return err ? "error" : "ok";
	}
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

/* Leaves the bytes of [off, off + len) in fs->comp, at *p. */
static int raw(struct squashfs *fs, uint64_t off, uint32_t len, const uint8_t **p)
{
	if (off > fs->bytes_used || len > fs->bytes_used - off)
		return SQE_RANGE;
	uint32_t head = (uint32_t)(off % EMMC_BLOCK), n = (head + len + EMMC_BLOCK - 1) / EMMC_BLOCK;
	if (n * EMMC_BLOCK > sizeof fs->comp)
		return SQE_RANGE;
	if (emmc_read(fs->first_lba + (uint32_t)(off / EMMC_BLOCK), n, fs->comp))
		return SQE_IO;
	*p = fs->comp + head;
	return 0;
}

static int meta_load(struct squashfs *fs, struct squashfs_mcache *c, uint64_t addr)
{
	if (c->valid && c->addr == addr)
		return 0;
	c->valid = false;
	if (addr >= fs->bytes_used)
		return SQE_RANGE;
	uint64_t avail = fs->bytes_used - addr;
	uint32_t n = avail < 2 + SQUASHFS_META_SIZE ? (uint32_t)avail : 2 + SQUASHFS_META_SIZE;
	const uint8_t *p;
	int r = raw(fs, addr, n, &p);
	if (r)
		return r;
	if (n < 2)
		return SQE_META;
	uint32_t hdr = le16(p), size = hdr & 0x7fff;
	if (!size || size > SQUASHFS_META_SIZE || size + 2 > n)
		return SQE_META;
	size_t len;
	if (hdr & 0x8000) {
		memcpy(c->data, p + 2, size);
		len = size;
	} else if (zlib_inflate(p + 2, size, c->data, SQUASHFS_META_SIZE, &len)) {
		return SQE_META;
	}
	if (!len)
		return SQE_META;
	c->addr = addr;
	c->next = addr + 2 + size;
	c->len = (uint32_t)len;
	c->valid = true;
	return 0;
}

static int meta_read(struct squashfs *fs, struct squashfs_mcache *c, uint64_t base, struct mpos *pos, void *dst, uint32_t n)
{
	uint8_t *d = dst;
	while (n) {
		int r = meta_load(fs, c, base + pos->block);
		if (r)
			return r;
		if (pos->off > c->len)
			return SQE_META;
		if (pos->off == c->len) {
			pos->block = c->next - base;
			pos->off = 0;
			continue;
		}
		uint32_t k = c->len - pos->off < n ? c->len - pos->off : n;
		memcpy(d, c->data + pos->off, k);
		d += k;
		n -= k;
		pos->off += k;
	}
	return 0;
}

static int iread(struct squashfs *fs, struct mpos *pos, void *dst, uint32_t n)
{
	return meta_read(fs, &fs->ic, fs->inode_table, pos, dst, n);
}

static int dread(struct squashfs *fs, struct mpos *pos, void *dst, uint32_t n)
{
	return meta_read(fs, &fs->dc, fs->dir_table, pos, dst, n);
}

static int read_inode(struct squashfs *fs, uint64_t ref, struct squashfs_inode *in)
{
	struct mpos pos = { ref >> 16, (uint32_t)(ref & 0xffff) };
	uint8_t h[16], b[40];
	if (pos.block > UINT32_MAX || pos.off >= SQUASHFS_META_SIZE)
		return SQE_INODE;
	int r = iread(fs, &pos, h, sizeof h);
	if (r)
		return r;
	unsigned type = le16(h);
	uint16_t perm = le16(h + 2) & 07777;
	memset(in, 0, sizeof *in);
	in->ino = le32(h + 12);
	in->ref = ref;
	in->frag = NO_FRAGMENT;
	unsigned kind = type > 7 ? type - 7 : type;
	switch (kind) {
	case 1:
		in->mode = SQUASHFS_S_IFDIR | perm;
		if (type == 1) {
			if ((r = iread(fs, &pos, b, 16)))
				return r;
			in->dir_block = le32(b);
			in->size = le16(b + 8);
			in->dir_off = le16(b + 10);
		} else {
			if ((r = iread(fs, &pos, b, 24)))
				return r;
			in->size = le32(b + 4);
			in->dir_block = le32(b + 8);
			in->dir_off = le16(b + 18);
		}
		if (in->dir_off >= SQUASHFS_META_SIZE)
			return SQE_INODE;
		return 0;
	case 2:
		in->mode = SQUASHFS_S_IFREG | perm;
		if (type == 2) {
			if ((r = iread(fs, &pos, b, 16)))
				return r;
			in->start = le32(b);
			in->frag = le32(b + 4);
			in->frag_off = le32(b + 8);
			in->size = le32(b + 12);
		} else {
			if ((r = iread(fs, &pos, b, 40)))
				return r;
			in->start = le64(b);
			in->size = le64(b + 8);
			in->frag = le32(b + 28);
			in->frag_off = le32(b + 32);
		}
		break;
	case 3:
		in->mode = SQUASHFS_S_IFLNK | perm;
		if ((r = iread(fs, &pos, b, 8)))
			return r;
		in->size = le32(b + 4);
		break;
	case 4: in->mode = 0x6000 | perm; return 0;
	case 5: in->mode = 0x2000 | perm; return 0;
	case 6: in->mode = 0x1000 | perm; return 0;
	case 7: in->mode = 0xc000 | perm; return 0;
	default:
		return SQE_INODE;
	}
	in->list_block = (uint32_t)pos.block;
	in->list_off = (uint16_t)pos.off;
	return 0;
}

static unsigned dirent_type(unsigned t)
{
	static const uint8_t map[8] = { 0, 2, 1, 7, 4, 3, 5, 6 };
	return map[(t > 7 ? t - 7 : t) & 7];
}

/* The callback returns 0 to go on, anything else ends the scan and is
 * returned. */
typedef int (*dent_fn)(const uint8_t *name, unsigned len, uint32_t ino, unsigned type, uint64_t ref, void *ctx);

static int dir_scan(struct squashfs *fs, const struct squashfs_inode *dir, dent_fn fn, void *ctx)
{
	if ((dir->mode & SQUASHFS_S_IFMT) != SQUASHFS_S_IFDIR)
		return SQE_NOTDIR;
	uint64_t left = dir->size < 3 ? 0 : dir->size - 3;
	struct mpos pos = { dir->dir_block, dir->dir_off };
	uint8_t h[12], e[8], name[256];
	while (left) {
		if (left < sizeof h)
			return SQE_DIRENT;
		int r = dread(fs, &pos, h, sizeof h);
		if (r)
			return r;
		left -= sizeof h;
		uint32_t count = le32(h) + 1, start = le32(h + 4), base = le32(h + 8);
		if (count > 256)
			return SQE_DIRENT;
		for (; count; count--) {
			if (left < sizeof e)
				return SQE_DIRENT;
			if ((r = dread(fs, &pos, e, sizeof e)))
				return r;
			left -= sizeof e;
			unsigned nlen = le16(e + 6) + 1u, off = le16(e);
			if (nlen > sizeof name || left < nlen || off >= SQUASHFS_META_SIZE)
				return SQE_DIRENT;
			if ((r = dread(fs, &pos, name, nlen)))
				return r;
			left -= nlen;
			uint32_t ino = base + (uint32_t)(int32_t)(int16_t)le16(e + 2);
			if ((r = fn(name, nlen, ino, dirent_type(le16(e + 4)), (uint64_t)start << 16 | off, ctx)))
				return r;
		}
	}
	return 0;
}

struct find {
	const char *name;
	size_t len;
	uint64_t ref;
};

static int find_fn(const uint8_t *name, unsigned len, uint32_t ino, unsigned type, uint64_t ref, void *ctx)
{
	struct find *f = ctx;
	if (len != f->len || memcmp(name, f->name, len))
		return 0;
	f->ref = ref;
	return 1;
}

static int dir_find(struct squashfs *fs, const struct squashfs_inode *dir, const char *name, size_t len, uint64_t *ref)
{
	struct find f = { name, len, 0 };
	int r = dir_scan(fs, dir, find_fn, &f);
	if (r < 0)
		return r;
	if (!r)
		return SQE_NOENT;
	*ref = f.ref;
	return 0;
}

struct list {
	squashfs_dir_fn fn;
	void *ctx;
};

static int list_fn(const uint8_t *name, unsigned len, uint32_t ino, unsigned type, uint64_t ref, void *ctx)
{
	struct list *l = ctx;
	char n[257];
	memcpy(n, name, len);
	n[len] = 0;
	l->fn(n, len, ino, type, l->ctx);
	return 0;
}

int squashfs_list(struct squashfs *fs, const struct squashfs_inode *dir, squashfs_dir_fn fn, void *ctx)
{
	struct list l = { fn, ctx };
	return dir_scan(fs, dir, list_fn, &l);
}

int squashfs_readlink(struct squashfs *fs, const struct squashfs_inode *in, char *buf, uint32_t cap)
{
	if ((in->mode & SQUASHFS_S_IFMT) != SQUASHFS_S_IFLNK)
		return SQE_SYMLINK;
	if (in->size >= cap)
		return SQE_TOOBIG;
	struct mpos pos = { in->list_block, in->list_off };
	int r = iread(fs, &pos, buf, (uint32_t)in->size);
	if (r)
		return r;
	buf[in->size] = 0;
	return (int)in->size;
}

/* The partition is mounted as /system on the phone, so the absolute links
 * in it point through that name; they are read relative to the volume's
 * own root unless the root really has a "system" entry. */
static bool has_entry(struct squashfs *fs, const char *name)
{
	struct squashfs_inode root;
	uint64_t ref;
	return !read_inode(fs, fs->root, &root) && !dir_find(fs, &root, name, strlen(name), &ref);
}

static int resolve(struct squashfs *fs, const char *path, bool follow_last, struct squashfs_inode *in)
{
	char rest[MAX_PATH], target[MAX_PATH];
	uint64_t stack[MAX_DEPTH];
	unsigned depth = 0, hops = 0;
	size_t n = strlen(path);
	if (n >= sizeof rest)
		return SQE_NAMETOOLONG;
	memcpy(rest, path, n + 1);
	const char *p = rest;
	stack[0] = fs->root;
	struct squashfs_inode cur;
	int r = read_inode(fs, fs->root, &cur);
	while (!r) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		const char *c = p;
		while (*p && *p != '/')
			p++;
		size_t clen = (size_t)(p - c);
		if (clen == 1 && c[0] == '.')
			continue;
		if (clen == 2 && c[0] == '.' && c[1] == '.') {
			if (depth)
				depth--;
			r = read_inode(fs, stack[depth], &cur);
			continue;
		}
		uint64_t ref;
		if (clen > 255)
			return SQE_NAMETOOLONG;
		if ((r = dir_find(fs, &cur, c, clen, &ref)) || (r = read_inode(fs, ref, &cur)))
			break;
		const char *q = p;
		while (*q == '/')
			q++;
		bool more = *q != 0;
		unsigned kind = cur.mode & SQUASHFS_S_IFMT;
		if (kind == SQUASHFS_S_IFLNK && (more || follow_last)) {
			if (++hops > MAX_HOPS)
				return SQE_LOOP;
			int tl = squashfs_readlink(fs, &cur, target, sizeof target);
			if (tl < 0)
				return tl;
			size_t rl = strlen(q);
			size_t t = strnlen(target, (size_t)tl), skip = 0;
			if (!t)
				return SQE_SYMLINK;
			if (t > 8 && !memcmp(target, "/system/", 8) && !has_entry(fs, "system"))
				skip = 7;
			t -= skip;
			if (skip + t + 1 + rl >= sizeof target)
				return SQE_NAMETOOLONG;
			target[skip + t] = '/';
			memcpy(target + skip + t + 1, q, rl + 1);
			memcpy(rest, target + skip, t + 2 + rl);
			if (rest[0] == '/')
				depth = 0;
			p = rest;
			r = read_inode(fs, stack[depth], &cur);
		} else if (kind == SQUASHFS_S_IFDIR) {
			if (depth + 1 >= MAX_DEPTH)
				return SQE_DEPTH;
			stack[++depth] = ref;
		} else if (more) {
			return SQE_NOTDIR;
		}
	}
	if (r)
		return r;
	*in = cur;
	return 0;
}

int squashfs_lookup(struct squashfs *fs, const char *path, struct squashfs_inode *in)
{
	return resolve(fs, path, true, in);
}

int squashfs_lookup_nofollow(struct squashfs *fs, const char *path, struct squashfs_inode *in)
{
	return resolve(fs, path, false, in);
}

static int decode_block(struct squashfs *fs, uint64_t disk, uint32_t word, uint8_t *dst, uint32_t cap, uint32_t *outlen)
{
	uint32_t csize = word & (UNCOMPRESSED_BLOCK - 1);
	const uint8_t *p;
	if (csize > fs->bs)
		return SQE_BLOCK;
	int r = raw(fs, disk, csize, &p);
	if (r)
		return r;
	if (word & UNCOMPRESSED_BLOCK) {
		if (csize > cap)
			return SQE_BLOCK;
		memcpy(dst, p, csize);
		*outlen = csize;
		return 0;
	}
	size_t len;
	if (zlib_inflate(p, csize, dst, cap, &len))
		return SQE_INFLATE;
	*outlen = (uint32_t)len;
	return 0;
}

/* Decodes into the shared block buffer unless that already holds the block. */
static int cached_block(struct squashfs *fs, uint64_t disk, uint32_t word, uint32_t want, bool exact)
{
	if (fs->blk_valid && fs->blk_addr == disk && (!exact || fs->blk_len == want))
		return 0;
	fs->blk_valid = false;
	uint32_t len;
	int r = decode_block(fs, disk, word, fs->blk, fs->bs, &len);
	if (r)
		return r;
	if (exact && len != want)
		return SQE_BLOCK;
	fs->blk_addr = disk;
	fs->blk_len = len;
	fs->blk_valid = true;
	return 0;
}

static int fragment_entry(struct squashfs *fs, uint32_t idx, uint64_t *disk, uint32_t *word)
{
	if (idx >= fs->fragments)
		return SQE_FRAGMENT;
	const uint8_t *p;
	int r = raw(fs, fs->frag_table + 8ull * (idx / FRAGS_PER_BLOCK), 8, &p);
	if (r)
		return r;
	struct mpos pos = { le64(p), (idx % FRAGS_PER_BLOCK) * 16 };
	uint8_t e[16];
	if ((r = meta_read(fs, &fs->fc, 0, &pos, e, sizeof e)))
		return r;
	*disk = le64(e);
	*word = le32(e + 8);
	return 0;
}

static int read_tail(struct squashfs *fs, const struct squashfs_inode *in, uint64_t from, uint64_t to, uint8_t *dst)
{
	uint64_t tail = in->size / fs->bs * fs->bs;
	uint64_t disk;
	uint32_t word;
	int r = fragment_entry(fs, in->frag, &disk, &word);
	if (r)
		return r;
	if ((r = cached_block(fs, disk, word, 0, false)))
		return r;
	if (in->frag_off > fs->blk_len || in->size - tail > fs->blk_len - in->frag_off)
		return SQE_FRAGMENT;
	memcpy(dst, fs->blk + in->frag_off + (from - tail), to - from);
	return 0;
}

int squashfs_read(struct squashfs *fs, const struct squashfs_inode *in, uint64_t off, void *buf, uint32_t len)
{
	if ((in->mode & SQUASHFS_S_IFMT) != SQUASHFS_S_IFREG)
		return SQE_NOTREG;
	if (len > INT32_MAX)
		return SQE_RANGE;
	if (off >= in->size)
		return 0;
	if (len > in->size - off)
		len = (uint32_t)(in->size - off);
	uint8_t *dst = buf;
	uint64_t end = off + len, bs = fs->bs, disk = in->start;
	uint64_t nblocks = in->frag == NO_FRAGMENT ? (in->size + bs - 1) / bs : in->size / bs;
	struct mpos pos = { in->list_block, in->list_off };
	for (uint64_t i = 0; i < nblocks && i * bs < end; i++) {
		uint8_t w[4];
		int r = iread(fs, &pos, w, sizeof w);
		if (r)
			return r;
		uint32_t word = le32(w), csize = word & (UNCOMPRESSED_BLOCK - 1);
		uint64_t lstart = i * bs, lend = in->size - lstart < bs ? in->size : lstart + bs;
		uint32_t want = (uint32_t)(lend - lstart);
		if (lend > off) {
			uint64_t from = off > lstart ? off : lstart, to = end < lend ? end : lend;
			uint8_t *d = dst + (from - off);
			if (!csize) {
				memset(d, 0, to - from);
			} else if (from == lstart && to == lend) {
				uint32_t got;
				if ((r = decode_block(fs, disk, word, d, want, &got)))
					return r;
				if (got != want)
					return SQE_BLOCK;
			} else {
				if ((r = cached_block(fs, disk, word, want, true)))
					return r;
				memcpy(d, fs->blk + (from - lstart), to - from);
			}
		}
		disk += csize;
	}
	uint64_t tail = nblocks * bs;
	if (in->frag != NO_FRAGMENT && end > tail && in->size > tail) {
		uint64_t from = off > tail ? off : tail;
		int r = read_tail(fs, in, from, end, dst + (from - off));
		if (r)
			return r;
	}
	return (int)len;
}

int squashfs_read_file(struct squashfs *fs, const char *path, void *buf, uint32_t cap)
{
	struct squashfs_inode in;
	int r = squashfs_lookup(fs, path, &in);
	if (r)
		return r;
	if ((in.mode & SQUASHFS_S_IFMT) != SQUASHFS_S_IFREG)
		return SQE_NOTREG;
	if (in.size > cap)
		return SQE_TOOBIG;
	r = squashfs_read(fs, &in, 0, buf, (uint32_t)in.size);
	if (r >= 0 && (uint64_t)r != in.size)
		return SQE_SHORT;
	return r;
}

int squashfs_open_lba(struct squashfs *fs, const char *name, uint32_t first_lba, uint32_t sectors)
{
	const uint8_t *sb;
	struct squashfs_inode root;
	fs->name = name;
	fs->first_lba = first_lba;
	fs->sectors = sectors;
	fs->bytes = fs->bytes_used = (uint64_t)sectors * EMMC_BLOCK;
	fs->ic.valid = fs->dc.valid = fs->fc.valid = fs->blk_valid = false;
	int r = raw(fs, 0, SB_SIZE, &sb);
	if (r)
		return r;
	if (le32(sb) != SQUASHFS_MAGIC)
		return SQE_MAGIC;
	if (le16(sb + 28) != 4 || le16(sb + 30) != 0)
		return SQE_VERSION;
	if (le16(sb + 20) != COMP_ZLIB)
		return SQE_COMPRESSOR;
	uint32_t bs = le32(sb + 12), log = le16(sb + 22);
	if (bs < 4096 || bs > SQUASHFS_MAX_BLOCK || (bs & (bs - 1)) || log > 20 || bs != 1u << log)
		return SQE_BLOCKSIZE;
	fs->bs = bs;
	fs->inodes = le32(sb + 4);
	fs->fragments = le32(sb + 16);
	fs->root = le64(sb + 32);
	fs->bytes_used = le64(sb + 40);
	fs->inode_table = le64(sb + 64);
	fs->dir_table = le64(sb + 72);
	fs->frag_table = le64(sb + 80);
	if (fs->bytes_used > fs->bytes || fs->bytes_used < SB_SIZE)
		return SQE_RANGE;
	if (fs->inode_table >= fs->bytes_used || fs->dir_table >= fs->bytes_used ||
	    (fs->fragments && fs->frag_table >= fs->bytes_used))
		return SQE_RANGE;
	if ((r = read_inode(fs, fs->root, &root)))
		return r;
	return (root.mode & SQUASHFS_S_IFMT) == SQUASHFS_S_IFDIR ? 0 : SQE_NOTDIR;
}
