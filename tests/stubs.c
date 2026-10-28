/* Host stand-ins for the hardware and log functions the console and adb
 * code call. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asteroid.h"
#include "bt/bt.h"
#include "stubs.h"

const char build_id[] = "hosttest";
uint32_t stub_ms = 12345, stub_rtc = 1000, stub_brightness;
bool stub_asleep;
jmp_buf *stub_reboot_jmp;
int stub_failures;

uint32_t time_ms(void) { return stub_ms; }
uint32_t timer_ticks(void) { return 42; }
uint32_t display_frames(void) { return 7; }
void display_set_brightness(uint8_t level) { stub_brightness = level; }
void display_sleep(void) { stub_asleep = true; }
void display_wake(void) { stub_asleep = false; }
uint32_t pmic_battery_mv(void) { return 3900; }
const char *pmic_charger_status(void) { return "discharging"; }
uint32_t pmic_rtc_read(void) { return stub_rtc; }
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
