/* Read-only ext4: superblock, group descriptors (32 or 64 bytes), inodes,
 * extent trees, linear directory scans (an htree directory's index blocks
 * look like empty entries, so a linear scan reads it too) and fast
 * symlinks. A volume sits either on a run of eMMC sectors or inside a
 * regular file of another volume (a loop image); ext4_walk turns a byte
 * range of a volume into eMMC sector runs through every level. Nothing
 * here writes. Features it cannot read are refused at open time, inodes
 * it cannot read (block maps, inline data, encryption) at use. */
#include <string.h>
#include "asteroid.h"
#include "emmc.h"
#include "ext4.h"

#define SB_OFFSET		1024u
#define EXT4_MAGIC		0xef53u
#define EXTENT_MAGIC		0xf30au
#define ROOT_INO		2u

#define INCOMPAT_FILETYPE	0x0002u
#define INCOMPAT_EXTENTS	0x0040u
#define INCOMPAT_64BIT		0x0080u
#define INCOMPAT_FLEX_BG	0x0200u
#define INCOMPAT_EA_INODE	0x0400u
#define INCOMPAT_CSUM_SEED	0x2000u
#define INCOMPAT_LARGEDIR	0x4000u
#define INCOMPAT_INLINE_DATA	0x8000u
#define INCOMPAT_ENCRYPT	0x10000u
#define INCOMPAT_CASEFOLD	0x20000u
#define INCOMPAT_READABLE	(INCOMPAT_FILETYPE | EXT4_INCOMPAT_RECOVER | INCOMPAT_EXTENTS | INCOMPAT_64BIT | \
				 INCOMPAT_FLEX_BG | INCOMPAT_EA_INODE | INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR | \
				 INCOMPAT_INLINE_DATA | INCOMPAT_ENCRYPT | INCOMPAT_CASEFOLD)

#define FL_ENCRYPT		0x00000800u
#define FL_EXTENTS		0x00080000u
#define FL_INLINE_DATA		0x10000000u

enum {
	E_IO = -100, E_RANGE, E_MAGIC, E_FEATURE, E_BLOCKSIZE, E_INODE, E_EXTENT, E_DEPTH, E_BLOCKMAP,
	E_INLINE, E_ENCRYPTED, E_NOTDIR, E_NOENT, E_DIRENT, E_LOOP, E_HOLE, E_ALIGN, E_SYMLINK,
};

const char *ext4_strerror(int err)
{
	switch (err) {
	case E_IO: return "eMMC read failed";
	case E_RANGE: return "outside the volume";
	case E_MAGIC: return "no ext4 superblock";
	case E_FEATURE: return "unsupported filesystem feature";
	case E_BLOCKSIZE: return "unsupported block size";
	case E_INODE: return "bad inode number";
	case E_EXTENT: return "corrupt extent tree";
	case E_DEPTH: return "extent tree too deep";
	case E_BLOCKMAP: return "inode uses block maps, not extents";
	case E_INLINE: return "inline data not supported";
	case E_ENCRYPTED: return "encrypted inode";
	case E_NOTDIR: return "not a directory";
	case E_NOENT: return "no such file or directory";
	case E_DIRENT: return "corrupt directory entry";
	case E_LOOP: return "too many symlinks";
	case E_HOLE: return "hole or unwritten extent";
	case E_ALIGN: return "unaligned access";
	case E_SYMLINK: return "unsupported symlink";
	default: return err ? "error" : "ok";
	}
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int walk(struct ext4 *fs, uint64_t off, uint64_t len, bool strict, ext4_seg_fn fn, void *ctx, uint32_t buf_off, int level)
{
	if ((off | len) & (EMMC_BLOCK - 1))
		return E_ALIGN;
	if (off > fs->bytes || len > fs->bytes - off)
		return E_RANGE;
	if (!fs->parent)
		return fn(fs->first_lba + (uint32_t)(off / EMMC_BLOCK), (uint32_t)(len / EMMC_BLOCK), buf_off, ctx);
	if (level > 4)
		return E_DEPTH;
	struct ext4 *p = fs->parent;
	while (len) {
		uint64_t lblk = off / p->bs, within = off % p->bs;
		if (!fs->cache.valid || lblk < fs->cache.lblk || lblk >= fs->cache.lblk + fs->cache.count) {
			uint64_t pblk, count;
			uint32_t flags;
			int r = ext4_map(p, &fs->backing, lblk, &pblk, &count, &flags);
			if (r)
				return r;
			fs->cache.lblk = lblk;
			fs->cache.pblk = pblk;
			fs->cache.count = count;
			fs->cache.flags = flags;
			fs->cache.valid = true;
		}
		uint64_t skip = lblk - fs->cache.lblk;
		uint64_t avail = (fs->cache.count - skip) * p->bs - within;
		uint64_t k = len < avail ? len : avail;
		int r;
		if (fs->cache.flags & (EXT4_MAP_HOLE | EXT4_MAP_UNWRITTEN)) {
			if (strict)
				return E_HOLE;
			r = fn(UINT32_MAX, (uint32_t)(k / EMMC_BLOCK), buf_off, ctx);
		} else {
			r = walk(p, (fs->cache.pblk + skip) * p->bs + within, k, strict, fn, ctx, buf_off, level + 1);
		}
		if (r)
			return r;
		off += k;
		len -= k;
		buf_off += (uint32_t)k;
	}
	return 0;
}

int ext4_walk(struct ext4 *fs, uint64_t off, uint64_t len, bool strict, ext4_seg_fn fn, void *ctx)
{
	return walk(fs, off, len, strict, fn, ctx, 0, 0);
}

static int read_seg(uint32_t lba, uint32_t n, uint32_t buf_off, void *ctx)
{
	uint8_t *buf = (uint8_t *)ctx + buf_off;
	if (lba == UINT32_MAX) {
		memset(buf, 0, n * EMMC_BLOCK);
		return 0;
	}
	return emmc_read(lba, n, buf) ? E_IO : 0;
}

static int vol_read(struct ext4 *fs, uint64_t off, uint32_t len, void *buf)
{
	return walk(fs, off, len, false, read_seg, buf, 0, 0);
}

static int open_common(struct ext4 *fs)
{
	uint8_t sb[1024];
	int r = vol_read(fs, SB_OFFSET, sizeof sb, sb);
	if (r)
		return r;
	if (le16(sb + 0x38) != EXT4_MAGIC)
		return E_MAGIC;
	uint32_t log = le32(sb + 0x18);
	if (log > 2)
		return E_BLOCKSIZE;
	fs->bs = 1024u << log;
	fs->compat = le32(sb + 0x5c);
	fs->incompat = le32(sb + 0x60);
	fs->ro_compat = le32(sb + 0x64);
	fs->default_mount_opts = le32(sb + 0x100);
	fs->state = le16(sb + 0x3a);
	fs->first_data_block = le32(sb + 0x14);
	fs->inodes = le32(sb + 0x00);
	fs->inodes_per_group = le32(sb + 0x28);
	fs->inode_size = le32(sb + 0x4c) ? le16(sb + 0x58) : 128;
	fs->desc_size = (fs->incompat & INCOMPAT_64BIT) ? le16(sb + 0xfe) : 32;
	fs->blocks = le32(sb + 0x04) | ((fs->incompat & INCOMPAT_64BIT) ? (uint64_t)le32(sb + 0x150) << 32 : 0);
	memcpy(fs->label, sb + 0x78, 16);
	fs->label[16] = 0;
	if (fs->incompat & ~INCOMPAT_READABLE)
		return E_FEATURE;
	if (!fs->inodes_per_group || fs->inode_size < 128 || fs->inode_size > EMMC_BLOCK || (fs->inode_size & (fs->inode_size - 1)) ||
	    fs->desc_size < 32 || fs->desc_size > 64 || (fs->desc_size & (fs->desc_size - 1)))
		return E_FEATURE;
	if (fs->blocks * fs->bs > fs->bytes)
		return E_RANGE;
	return 0;
}

int ext4_open_lba(struct ext4 *fs, const char *name, uint32_t first_lba, uint32_t sectors)
{
	memset(fs, 0, sizeof *fs);
	fs->name = name;
	fs->first_lba = first_lba;
	fs->sectors = sectors;
	fs->bytes = (uint64_t)sectors * EMMC_BLOCK;
	return open_common(fs);
}

int ext4_open_file(struct ext4 *fs, const char *name, struct ext4 *parent, const struct ext4_inode *file)
{
	memset(fs, 0, sizeof *fs);
	fs->name = name;
	fs->parent = parent;
	fs->backing = *file;
	fs->bytes = file->size & ~(uint64_t)(EMMC_BLOCK - 1);
	return open_common(fs);
}

int ext4_read_inode(struct ext4 *fs, uint32_t ino, struct ext4_inode *in)
{
	if (!ino || ino > fs->inodes)
		return E_INODE;
	uint32_t group = (ino - 1) / fs->inodes_per_group, index = (ino - 1) % fs->inodes_per_group;
	uint8_t s[EMMC_BLOCK];
	uint64_t d = (uint64_t)(fs->first_data_block + 1) * fs->bs + (uint64_t)group * fs->desc_size;
	int r = vol_read(fs, d & ~(uint64_t)(EMMC_BLOCK - 1), EMMC_BLOCK, s);
	if (r)
		return r;
	const uint8_t *g = s + (d & (EMMC_BLOCK - 1));
	uint64_t table = le32(g + 8);
	if (fs->desc_size >= 64)
		table |= (uint64_t)le32(g + 0x28) << 32;
	uint64_t at = table * fs->bs + (uint64_t)index * fs->inode_size;
	if ((r = vol_read(fs, at & ~(uint64_t)(EMMC_BLOCK - 1), EMMC_BLOCK, s)))
		return r;
	const uint8_t *p = s + (at & (EMMC_BLOCK - 1));
	in->ino = ino;
	in->mode = le16(p + 0x00);
	in->links = le16(p + 0x1a);
	in->size = le32(p + 0x04) | (uint64_t)le32(p + 0x6c) << 32;
	in->flags = le32(p + 0x20);
	memcpy(in->iblock, p + 0x28, sizeof in->iblock);
	return 0;
}

static int check_readable(const struct ext4_inode *in)
{
	if (in->flags & FL_INLINE_DATA)
		return E_INLINE;
	if (in->flags & FL_ENCRYPT)
		return E_ENCRYPTED;
	if (!(in->flags & FL_EXTENTS))
		return E_BLOCKMAP;
	return 0;
}

int ext4_map(struct ext4 *fs, const struct ext4_inode *in, uint64_t lblk, uint64_t *pblk, uint64_t *count, uint32_t *flags)
{
	int r = check_readable(in);
	if (r)
		return r;
	const uint8_t *n = in->iblock;
	uint32_t size = sizeof in->iblock;
	int depth = -1;
	for (;;) {
		uint16_t entries = le16(n + 2), max = le16(n + 4), d = le16(n + 6);
		if (le16(n) != EXTENT_MAGIC || entries > max || 12u + (uint32_t)max * 12 > size)
			return E_EXTENT;
		if (depth >= 0 && d != depth - 1)
			return E_EXTENT;
		if (d > 5)
			return E_DEPTH;
		depth = d;
		if (!d)
			break;
		int pick = -1;
		for (unsigned i = 0; i < entries; i++) {
			if (le32(n + 12 + i * 12) <= lblk)
				pick = (int)i;
			else
				break;
		}
		if (pick < 0) {
			*pblk = 0;
			*count = entries ? le32(n + 12) - lblk : 1;
			*flags = EXT4_MAP_HOLE;
			return 0;
		}
		const uint8_t *e = n + 12 + pick * 12;
		uint64_t child = le32(e + 4) | (uint64_t)le16(e + 8) << 32;
		if ((r = vol_read(fs, child * fs->bs, fs->bs, fs->node)))
			return r;
		n = fs->node;
		size = fs->bs;
	}
	uint16_t entries = le16(n + 2);
	uint64_t next = 0;
	for (unsigned i = 0; i < entries; i++) {
		const uint8_t *e = n + 12 + i * 12;
		uint32_t start = le32(e);
		uint32_t len = le16(e + 4);
		uint32_t f = 0;
		if (len > 32768) {
			len -= 32768;
			f = EXT4_MAP_UNWRITTEN;
		}
		if (lblk < start) {
			next = start;
			break;
		}
		if (lblk < (uint64_t)start + len) {
			*pblk = (le32(e + 8) | (uint64_t)le16(e + 6) << 32) + (lblk - start);
			*count = start + len - lblk;
			*flags = f;
			return 0;
		}
	}
	*pblk = 0;
	*count = next > lblk ? next - lblk : 1;
	*flags = EXT4_MAP_HOLE;
	return 0;
}

int ext4_read(struct ext4 *fs, const struct ext4_inode *in, uint64_t off, void *buf, uint32_t len)
{
	uint8_t *out = buf;
	if (off >= in->size)
		return 0;
	if (len > in->size - off)
		len = (uint32_t)(in->size - off);
	uint32_t done = 0;
	while (done < len) {
		uint64_t lblk = (off + done) / fs->bs, pblk, count;
		uint32_t within = (uint32_t)((off + done) % fs->bs), flags;
		int r = ext4_map(fs, in, lblk, &pblk, &count, &flags);
		if (r)
			return r;
		uint32_t k = fs->bs - within;
		if (k > len - done)
			k = len - done;
		if (flags) {
			memset(out + done, 0, k);
		} else {
			if ((r = vol_read(fs, pblk * fs->bs, fs->bs, fs->blk)))
				return r;
			memcpy(out + done, fs->blk + within, k);
		}
		done += k;
	}
	return (int)done;
}

typedef bool (*dirent_fn)(const uint8_t *name, unsigned len, uint32_t ino, unsigned type, void *ctx);

static int scan_dir(struct ext4 *fs, const struct ext4_inode *dir, dirent_fn fn, void *ctx)
{
	if ((dir->mode & EXT4_S_IFMT) != EXT4_S_IFDIR)
		return E_NOTDIR;
	if (dir->size > fs->bytes)
		return E_DIRENT;
	uint64_t nblocks = (dir->size + fs->bs - 1) / fs->bs;
	for (uint64_t lblk = 0; lblk < nblocks; lblk++) {
		uint64_t pblk, count;
		uint32_t flags;
		int r = ext4_map(fs, dir, lblk, &pblk, &count, &flags);
		if (r)
			return r;
		if (flags) {
			lblk += count - 1;
			continue;
		}
		if ((r = vol_read(fs, pblk * fs->bs, fs->bs, fs->blk)))
			return r;
		for (uint32_t o = 0; o + 8 <= fs->bs;) {
			const uint8_t *d = fs->blk + o;
			uint32_t ino = le32(d);
			uint16_t rec = le16(d + 4);
			uint8_t nlen = d[6];
			if (rec < 8 || (rec & 3) || o + rec > fs->bs || 8u + nlen > rec)
				return E_DIRENT;
			if (ino && nlen && fn(d + 8, nlen, ino, (fs->incompat & INCOMPAT_FILETYPE) ? d[7] : 0, ctx))
				return 0;
			o += rec;
		}
		wdt_feed();
	}
	return 0;
}

struct find { const char *name; unsigned len; uint32_t ino; };

static bool find_fn(const uint8_t *name, unsigned len, uint32_t ino, unsigned type, void *ctx)
{
	struct find *f = ctx;
	if (len != f->len || memcmp(name, f->name, len))
		return false;
	f->ino = ino;
	return true;
}

static int read_symlink(struct ext4 *fs, const struct ext4_inode *in, char *buf, unsigned size)
{
	if (in->size >= size)
		return E_SYMLINK;
	unsigned n = (unsigned)in->size;
	if (!(in->flags & (FL_EXTENTS | FL_INLINE_DATA)) && n < sizeof in->iblock) {
		memcpy(buf, in->iblock, n);
	} else {
		int r = ext4_read(fs, in, 0, buf, n);
		if (r < 0)
			return r;
		if ((unsigned)r != n)
			return E_SYMLINK;
	}
	buf[n] = 0;
	return 0;
}

int ext4_lookup(struct ext4 *fs, const char *path, struct ext4_inode *in)
{
	char buf[2][256];
	int cur = 0, links = 0;
	size_t pl = strlen(path);
	if (pl >= sizeof buf[0])
		return E_NOENT;
	memcpy(buf[cur], path, pl + 1);
	struct ext4_inode dir;
	int r = ext4_read_inode(fs, ROOT_INO, &dir);
	if (r)
		return r;
	const char *p = buf[cur];
	for (;;) {
		while (*p == '/')
			p++;
		if (!*p) {
			*in = dir;
			return 0;
		}
		const char *e = p;
		while (*e && *e != '/')
			e++;
		struct find f = { p, (unsigned)(e - p), 0 };
		if (f.len == 1 && *p == '.') {
			p = e;
			continue;
		}
		if ((r = scan_dir(fs, &dir, find_fn, &f)))
			return r;
		if (!f.ino)
			return E_NOENT;
		struct ext4_inode next;
		if ((r = ext4_read_inode(fs, f.ino, &next)))
			return r;
		if ((next.mode & EXT4_S_IFMT) == EXT4_S_IFLNK) {
			if (++links > 8)
				return E_LOOP;
			char target[256];
			if ((r = read_symlink(fs, &next, target, sizeof target)))
				return r;
			size_t tl = strlen(target), rl = strlen(e);
			if (tl + rl + 1 >= sizeof buf[0])
				return E_SYMLINK;
			int other = cur ^ 1;
			memcpy(buf[other], target, tl);
			memcpy(buf[other] + tl, e, rl + 1);
			cur = other;
			p = buf[cur];
			if (*p == '/' && (r = ext4_read_inode(fs, ROOT_INO, &dir)))
				return r;
			continue;
		}
		dir = next;
		p = e;
	}
}

struct list { ext4_dir_fn fn; void *ctx; };

static bool list_fn(const uint8_t *name, unsigned len, uint32_t ino, unsigned type, void *ctx)
{
	struct list *l = ctx;
	char n[256];
	memcpy(n, name, len);
	n[len] = 0;
	l->fn(n, len, ino, type, l->ctx);
	return false;
}

int ext4_list(struct ext4 *fs, const struct ext4_inode *dir, ext4_dir_fn fn, void *ctx)
{
	struct list l = { fn, ctx };
	return scan_dir(fs, dir, list_fn, &l);
}
