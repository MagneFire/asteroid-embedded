#ifndef STORAGE_H
#define STORAGE_H
#include <stdint.h>
#include <stdbool.h>
#include "emmc.h"
#include "ext4.h"

#define STORE_PARTITION		EMMC_WRITE_PARTITION
#define STORE_MIN_SIZE		(36u * 1024)
#define STORE_MAX_SIZE		(1024u * 1024)

struct store_seg { uint32_t off, lba, count; };

struct store_info {
	bool open, writable;
	const char *why;
	uint32_t size;
	unsigned nsegs;
	struct store_seg segs[16];
};

int storage_mount(void);
struct ext4 *storage_userdata(void);
struct ext4 *storage_rootfs(void);
const char *storage_rootfs_where(void);
struct ext4 *storage_resolve(const char *path, const char **rest);

int store_open(void);
const struct store_info *store_info(void);
int store_read(uint32_t off, void *buf, uint32_t len);
int store_write(uint32_t off, const void *buf, uint32_t len);

#define KV_SLOT_SIZE		(16u * 1024)
#define KV_KEY_MAX		32u
#define KV_VALUE_MAX		1024u
int kv_load(void);
int kv_get(const char *key, void *buf, unsigned size);
int kv_set(const char *key, const void *val, unsigned len);
int kv_del(const char *key);
typedef void (*kv_each_fn)(const char *key, const uint8_t *val, unsigned len, void *ctx);
int kv_each(kv_each_fn fn, void *ctx);
uint32_t kv_seq(void);
#endif
