/* The files adb push/pull see: a fixed table of named RAM buffers, gone
 * on reboot. */
#include <string.h>
#include "asteroid.h"
#include "adb.h"

static struct adb_file files[ADB_FILES];

struct adb_file *adb_fs_find(const char *name)
{
	for (unsigned i = 0; i < ADB_FILES; i++)
		if (files[i].used && !strcmp(files[i].name, name))
			return &files[i];
	return NULL;
}

struct adb_file *adb_fs_create(const char *name, uint32_t mode)
{
	size_t n = strlen(name);
	if (n >= ADB_NAME_MAX)
		return NULL;
	struct adb_file *f = adb_fs_find(name);
	for (unsigned i = 0; !f && i < ADB_FILES; i++)
		if (!files[i].used)
			f = &files[i];
	if (!f)
		return NULL;
	memcpy(f->name, name, n + 1);
	f->mode = mode;
	f->mtime = 0;
	f->size = 0;
	f->used = true;
	return f;
}

struct adb_file *adb_fs_entry(unsigned i)
{
	return i < ADB_FILES && files[i].used ? &files[i] : NULL;
}

void adb_fs_remove(struct adb_file *f)
{
	f->used = false;
}

bool adb_fs_write(const char *name, uint32_t mode, uint32_t mtime, const void *data, uint32_t size)
{
	if (size > ADB_FILE_MAX)
		return false;
	struct adb_file *f = adb_fs_create(name, mode);
	if (!f)
		return false;
	memcpy(f->data, data, size);
	f->size = size;
	f->mtime = mtime;
	return true;
}
