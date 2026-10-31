#ifndef ADB_H
#define ADB_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define ADB_MAX_PAYLOAD	4096
#define ADB_RX_MAX	16384
#define ADB_FILES	8
#define ADB_FILE_MAX	65536
#define ADB_NAME_MAX	128

/* send() hands one bulk-IN transfer to the USB side, which is done with
 * buf when it returns. It returns false while the previous transfer is
 * still in flight; adb retries the same transfer on a later adb_poll().
 * pump(), if given, runs the USB side once (transfers both ways): a
 * console command running from the main loop waits in it for the host
 * to take its output when a stream's ring is full. */
struct adb_transport {
	bool (*send)(const void *buf, uint32_t len);
	void (*pump)(void);
};

void adb_init(const struct adb_transport *t, const char *banner_props);
void adb_rx(const void *buf, uint32_t len);
uint32_t adb_rx_expected(void);
void adb_poll(void);
bool adb_online(void);
bool adb_pending(void);
void adb_reset(void);

struct adb_file {
	char name[ADB_NAME_MAX];
	uint32_t mode, mtime, size;
	bool used;
	uint8_t data[ADB_FILE_MAX];
};

struct adb_file *adb_fs_find(const char *name);
struct adb_file *adb_fs_create(const char *name, uint32_t mode);
struct adb_file *adb_fs_entry(unsigned i);
void adb_fs_remove(struct adb_file *f);
bool adb_fs_write(const char *name, uint32_t mode, uint32_t mtime, const void *data, uint32_t size);
#endif
