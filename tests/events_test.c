/* The event history on the host: lines are appended with the RTC time,
 * the oldest go first when the store's value is full, and nothing is
 * written before the store answers. */
#include <stdio.h>
#include <string.h>
#include "asteroid.h"
#include "storage/storage.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static int store_state;		/* -1: no store, 0: empty, 1: has value */
static char stored[2048];
static unsigned stored_len;
static int sets;
static bool fake_cable, fake_usb, fake_lost;
static uint32_t fake_rtc = 1000;
static char logged[4096];

int kv_get(const char *key, void *buf, unsigned size)
{
	if (store_state < 0)
		return -1;
	if (store_state == 0)
		return -2;
	memcpy(buf, stored, stored_len < size ? stored_len : size);
	return (int)stored_len;
}
int kv_set(const char *key, const void *val, unsigned len)
{
	memcpy(stored, val, len);
	stored_len = len;
	store_state = 1;
	sets++;
	return 0;
}
bool batt_cable(void) { return fake_cable; }
bool batt_usb(void) { return fake_usb; }
uint32_t rtc_read(void) { return fake_rtc; }
const char *boot_reason(void) { return "pon 02 smpl"; }
bool log_ring_lost(void) { return fake_lost; }
void log_puts(const char *s) { strncat(logged, s, sizeof logged - strlen(logged) - 1); }
void log_printf(const char *fmt, ...) {}

int main(void)
{
	char buf[32] = "";
	events_append(buf, sizeof buf, "first line");
	events_append(buf, sizeof buf, "second");
	CHECK(!strcmp(buf, "first line\nsecond\n"));
	events_append(buf, sizeof buf, "third line here");
	CHECK(!strcmp(buf, "second\nthird line here\n"));
	events_append(buf, sizeof buf, "a line longer than the whole buffer is");
	CHECK(!strcmp(buf, "second\nthird line here\n"));
	events_append(buf, sizeof buf, "0123456789012345678901234567");
	CHECK(!strcmp(buf, "0123456789012345678901234567\n"));

	/* No store yet: nothing written, retried later. */
	store_state = -1;
	events_poll(0);
	events_poll(1000);
	CHECK(sets == 0);
	/* The store answers: the earlier lines are logged, the boot noted. */
	store_state = 1;
	strcpy(stored, "900 cable 1 usb 1\n");
	stored_len = (unsigned)strlen(stored);
	fake_cable = true;
	fake_usb = true;
	fake_lost = true;
	events_poll(6000);
	CHECK(sets == 1);
	CHECK(strstr(logged, "900 cable 1 usb 1\n"));
	CHECK(!strncmp(stored, "900 cable 1 usb 1\n1000 boot cable 1 usb 1, pon 02 smpl, no log from the previous boot\n", stored_len));
	/* Nothing changes: nothing written. */
	events_poll(7000);
	events_poll(8000);
	CHECK(sets == 1);
	/* The dock goes, within the sample period it is not seen yet. */
	fake_cable = false;
	fake_rtc = 1200;
	events_poll(8500);
	CHECK(sets == 1);
	events_poll(9000);
	CHECK(sets == 2);
	CHECK(strstr(stored, "\n1200 cable 0 usb 1\n"));

	printf(failures ? "events_test: %d failures\n" : "events_test: ok\n", failures);
	return failures != 0;
}
