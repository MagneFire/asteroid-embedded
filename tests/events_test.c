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
static int sets, reads, set_result;
static bool quiet = true;
static uint32_t quiet_wait;
static bool fake_cable, fake_usb, fake_lost;
static uint32_t fake_rtc = 1000;
static char logged[4096];

int kv_get(const char *key, void *buf, unsigned size)
{
	reads++;
	if (store_state < 0)
		return -1;
	if (store_state == 0)
		return -2;
	memcpy(buf, stored, stored_len < size ? stored_len : size);
	return (int)stored_len;
}
int kv_set(const char *key, const void *val, unsigned len)
{
	sets++;
	if (set_result)
		return set_result;
	memcpy(stored, val, len);
	stored_len = len;
	store_state = 1;
	return 0;
}
bool batt_cable(void) { return fake_cable; }
bool batt_usb(void) { return fake_usb; }
uint32_t rtc_read(void) { return fake_rtc; }
const char *boot_reason(void) { return "pon 02 smpl"; }
bool log_ring_lost(void) { return fake_lost; }
bool store_quiet(void) { return quiet; }
uint32_t store_quiet_wait_ms(void) { return quiet ? 0 : quiet_wait; }
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

	/* No store yet: asked at the first pass, nothing written, retried
	 * later. A line before the store answers is dropped. */
	store_state = -1;
	events_poll(0);
	CHECK(reads == 1);
	events_poll(1000);
	events_note("too early");
	CHECK(sets == 0 && reads == 1 && events_next_ms(1000) == UINT32_MAX);
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
	CHECK(!strstr(stored, "too early") && events_next_ms(9000) == UINT32_MAX);

	/* A noted line waits for the store to be quiet, and says when to
	 * come back for it. */
	quiet = false;
	quiet_wait = 3000;
	fake_rtc = 1300;
	events_note("wristband mode");
	CHECK(events_next_ms(9100) == 3000);
	events_poll(10000);
	events_poll(11000);
	CHECK(sets == 2);
	quiet_wait = 200;
	CHECK(events_next_ms(11100) == 900);
	quiet = true;
	CHECK(events_next_ms(11100) == 900 && events_next_ms(12000) == 1);
	events_poll(12000);
	CHECK(sets == 3 && strstr(stored, "\n1300 wristband mode\n") && events_next_ms(12000) == UINT32_MAX);

	/* Before a reset it is written whether the store is quiet or not,
	 * and only if something waits. */
	quiet = false;
	events_note("power off mode");
	events_poll(13000);
	CHECK(sets == 3);
	events_flush();
	CHECK(sets == 4 && strstr(stored, "\n1300 power off mode\n"));
	events_flush();
	events_poll(14000);
	CHECK(sets == 4 && events_next_ms(14000) == UINT32_MAX);

	/* A store that does not take the line is tried again a minute
	 * later, not at every sample. */
	quiet = true;
	set_result = -20;
	events_note("bt connected");
	events_poll(15000);
	CHECK(sets == 5 && events_next_ms(15000) == 60000);
	events_poll(16000);
	events_poll(74000);
	CHECK(sets == 5 && events_next_ms(74000) == 1000);
	set_result = 0;
	events_poll(75000);
	CHECK(sets == 6 && strstr(stored, "\n1300 bt connected\n") && events_next_ms(75000) == UINT32_MAX);

	printf(failures ? "events_test: %d failures\n" : "events_test: ok\n", failures);
	return failures != 0;
}
