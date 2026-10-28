#ifndef STORAGE_H
#define STORAGE_H
#include <stdint.h>
#include <stdbool.h>
#include "emmc.h"
#include "ext4.h"

int storage_mount(void);
struct ext4 *storage_userdata(void);
struct ext4 *storage_rootfs(void);
const char *storage_rootfs_where(void);
struct ext4 *storage_resolve(const char *path, const char **rest);
#endif
