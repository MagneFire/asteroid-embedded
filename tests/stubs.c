/* Host stand-ins for the hardware and log functions the console and adb
 * code call. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asteroid.h"
#include "bt/bt.h"
#include "stubs.h"
#include "storage/storage.h"

const char build_id[] = "hosttest";
uint32_t stub_ms = 12345, stub_rtc = 1000, stub_brightness;
bool stub_asleep;
jmp_buf *stub_reboot_jmp;
int stub_failures;

uint32_t time_ms(void) { return stub_ms; }
uint32_t time_us(void) { return stub_ms * 1000; }
uint32_t timer_ticks(void) { return 42; }
uint32_t display_frames(void) { return 7; }
void display_set_brightness(uint8_t level) { stub_brightness = level; }
void display_sleep(void) { stub_asleep = true; }
void display_wake(void) { stub_asleep = false; }
uint32_t pmic_battery_mv(void) { return 3900; }
const char *pmic_charger_status(void) { return "discharging"; }
uint32_t pmic_rtc_read(void) { return stub_rtc; }
uint8_t pmic_read(uint32_t addr) { return 0; }
void pmic_rtc_write(uint32_t secs) { stub_rtc = secs; }
uint32_t readl(uint32_t a) { return a ^ 0xdeadbeefu; }
int bt_start(const uint8_t *bdaddr, struct bt_info *info)
{
	memset(info, 0, sizeof *info);
	info->failed = "host test";
	return -1;
}
void bt_stop(void) {}
bool bt_running(void) { return false; }
bool bt_host_wake(void) { return false; }
int bt_host_start(void) { return -1; }
void bt_host_stop(void) {}
int bt_store_clear(void) { return -1; }
void haptics_play(const uint16_t *on_off_ms, unsigned n) {}
void cpu_clock_slow(bool slow) {}
bool cpu_clock_is_slow(void) { return false; }
void power_request(bool sleep) { stub_asleep = sleep; }
void hub_status(void (*say)(const char *fmt, ...)) {}
int hub_send(const uint8_t *p, unsigned n) { return -1; }
void touch_inject(int x0, int y0, int x1, int y1, uint32_t ms) {}
void screenshot_to_file(int layer) {}
void bt_host_status(struct bt_status *s) { memset(s, 0, sizeof *s); s->kv = "none"; s->next_ms = UINT32_MAX; }
int bt_addr_load(uint8_t addr[6]) { return -1; }
int bt_addr_save(const uint8_t addr[6]) { return -1; }

void reboot(void)
{
	if (stub_reboot_jmp)
		longjmp(*stub_reboot_jmp, 1);
	fprintf(stderr, "unexpected reboot\n");
	abort();
}

void log_puts(const char *s) { fputs(s, stderr); }

void log_printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	fputs(buf, stderr);
}

void log_grep(const char *pat, unsigned lines, void (*out)(const char *s, size_t n, void *ctx), void *ctx)
{
}

void log_tail(unsigned lines, void (*out)(const char *s, size_t n, void *ctx), void *ctx)
{
	static const char *log[] = { "line one\n", "line two\n", "line three\n" };
	for (unsigned i = lines < 3 ? 3 - lines : 0; i < 3; i++)
		out(log[i], strlen(log[i]), ctx);
}

static struct emmc_info stub_emmc;
static struct store_info stub_store;
int emmc_init(bool fast) { return -1; }
const struct emmc_info *emmc_card(void) { return &stub_emmc; }
int emmc_read(uint32_t lba, uint32_t n, void *buf) { return -1; }
int emmc_part_scan(void) { return -1; }
unsigned emmc_part_count(void) { return 0; }
const struct emmc_part *emmc_part_get(unsigned i) { return NULL; }
int emmc_part_find(const char *name, uint32_t *first, uint32_t *count) { return -1; }
int storage_mount(void) { return -1; }
struct ext4 *storage_userdata(void) { return NULL; }
struct ext4 *storage_rootfs(void) { return NULL; }
const char *storage_rootfs_where(void) { return "not mounted"; }
struct ext4 *storage_resolve(const char *path, const char **rest) { return NULL; }
int ext4_lookup(struct ext4 *fs, const char *path, struct ext4_inode *in) { return -1; }
int ext4_read(struct ext4 *fs, const struct ext4_inode *in, uint64_t off, void *buf, uint32_t len) { return -1; }
int ext4_list(struct ext4 *fs, const struct ext4_inode *dir, ext4_dir_fn fn, void *ctx) { return -1; }
const char *ext4_strerror(int err) { return "error"; }
int store_open(void) { return -1; }
const struct store_info *store_info(void) { return &stub_store; }
int store_read(uint32_t off, void *buf, uint32_t len) { return -1; }
int store_write(uint32_t off, const void *buf, uint32_t len) { return -1; }
int kv_get(const char *key, void *buf, unsigned size) { return -1; }
int kv_set(const char *key, const void *val, unsigned len) { return -1; }
int kv_del(const char *key) { return -1; }
int kv_each(kv_each_fn fn, void *ctx) { return -1; }
uint32_t kv_seq(void) { return 0; }
