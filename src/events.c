/* A short history of boots and charger changes in the kv store, one line
 * per event with the RTC time. A death that takes DRAM with it, as the
 * supply loss seen on smelt did, leaves no log at all; this still says
 * when the watch last booted, what the PMIC gave as the reason, and when
 * the dock or USB last changed. It is written on a change only, so it
 * costs nothing while the watch sleeps. The lines are read back at the
 * first pass of the main loop, when the store is open, and logged. A
 * line from events_note waits for the store to be quiet, and the main
 * loop wakes for that. */
#include <string.h>
#include "asteroid.h"
#include "storage/storage.h"

#define KV_EVENTS	"events"
#define EVENTS_MAX	800u
#define SAMPLE_MS	1000u
#define RETRY_MS	5000u
/* A page that does not take the write is not erased every second. */
#define SAVE_RETRY_MS	60000u

static char history[EVENTS_MAX + 1];
static bool loaded, tried, unsaved, failed;
static uint32_t sample_t, retry_t, failed_t;
static int cable = -1, usb = -1;

/* Appends one line to buf, dropping the oldest lines until it fits in
 * cap bytes with its terminator. */
void events_append(char *buf, unsigned cap, const char *line)
{
	unsigned n = (unsigned)strlen(line) + 1;
	if (n + 1 > cap)
		return;
	while (strlen(buf) + n + 1 > cap) {
		char *nl = strchr(buf, '\n');
		if (!nl) {
			buf[0] = 0;
			break;
		}
		memmove(buf, nl + 1, strlen(nl + 1) + 1);
	}
	unsigned at = (unsigned)strlen(buf);
	memcpy(buf + at, line, n - 1);
	buf[at + n - 1] = '\n';
	buf[at + n] = 0;
}

static void save(uint32_t now)
{
	unsaved = failed = kv_set(KV_EVENTS, history, (unsigned)strlen(history)) != 0;
	failed_t = now;
}

static void note(const char *what, uint32_t now)
{
	char line[320];
	snprintf(line, sizeof line, "%lu %s", rtc_read(), what);
	events_append(history, EVENTS_MAX, line);
	save(now);
}

/* For the rare events worth keeping across a reset that loses the log,
 * such as the AP's mode switches and BLE coming up. */
void events_note(const char *what)
{
	if (!loaded)
		return;
	char line[320];
	snprintf(line, sizeof line, "%lu %s", rtc_read(), what);
	events_append(history, EVENTS_MAX, line);
	unsaved = true;
	failed = false;
}

void events_flush(void)
{
	if (loaded && unsaved)
		unsaved = kv_set(KV_EVENTS, history, (unsigned)strlen(history)) != 0;
}

uint32_t events_next_ms(uint32_t now)
{
	if (!loaded || !unsaved)
		return UINT32_MAX;
	uint32_t sample = ms_left(sample_t, SAMPLE_MS, now);
	uint32_t wait = failed ? ms_left(failed_t, SAVE_RETRY_MS, now) : store_quiet_wait_ms();
	return sample > wait ? sample : wait;
}

static bool load(uint32_t now)
{
	if (tried && now - retry_t < RETRY_MS)
		return false;
	tried = true;
	retry_t = now;
	int n = kv_get(KV_EVENTS, history, EVENTS_MAX);
	if (n == -1)
		return false;	/* no store yet */
	if (n < 0)
		n = 0;
	history[n < (int)EVENTS_MAX ? n : (int)EVENTS_MAX] = 0;
	if (history[0]) {
		log_puts("events before this boot:\n");
		log_puts(history);
	}
	return true;
}

void events_poll(uint32_t now)
{
	if (!loaded) {
		if (!load(now))
			return;
		loaded = true;
		cable = batt_cable();
		usb = batt_usb();
		char what[300];
		snprintf(what, sizeof what, "boot cable %d usb %d, %s%s", cable, usb, boot_reason(),
			 log_ring_lost() ? ", no log from the previous boot" : "");
		note(what, now);
		sample_t = now;
		return;
	}
	if (now - sample_t < SAMPLE_MS)
		return;
	sample_t = now;
	if (unsaved && store_quiet() && (!failed || now - failed_t >= SAVE_RETRY_MS))
		save(now);
	int c = batt_cable(), u = batt_usb();
	if (c == cable && u == usb)
		return;
	cable = c;
	usb = u;
	char what[32];
	snprintf(what, sizeof what, "cable %d usb %d", c, u);
	note(what, now);
}
