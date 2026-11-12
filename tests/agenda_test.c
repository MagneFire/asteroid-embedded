/* The Agenda's events on the host, over an in-memory key-value store:
 * the order, edits that move an event, the checks, a full agenda, the
 * titles, the days and what comes back from the store. */
#include <stdio.h>
#include <string.h>
#include "asteroid.h"
#include "storage/storage.h"
#include "agenda.h"
#include "stubs.h"

static uint8_t stored[KV_VALUE_MAX];
static int stored_len = -2;
static unsigned kv_writes;

int kv_get(const char *key, void *buf, unsigned size)
{
	if (strcmp(key, "agenda") || stored_len < 0)
		return stored_len < 0 ? stored_len : -2;
	memcpy(buf, stored, (unsigned)stored_len < size ? (unsigned)stored_len : size);
	return stored_len;
}

int kv_set(const char *key, const void *val, unsigned len)
{
	CHECK(!strcmp(key, "agenda") && len <= KV_VALUE_MAX);
	kv_writes++;
	memcpy(stored, val, len);
	stored_len = (int)len;
	return 0;
}

static struct agenda_event ev(unsigned y, unsigned m, unsigned d, unsigned start, unsigned end, const char *title)
{
	struct agenda_event e = { (uint16_t)y, (uint8_t)m, (uint8_t)d, (uint16_t)start, (uint16_t)end, "" };
	strncpy(e.title, title, AGENDA_TITLE);
	return e;
}

static const char *title(unsigned i)
{
	const struct agenda_event *e = agenda_get(i);
	return e ? e->title : "";
}

int main(void)
{
	/* What was stored comes back, its titles ended. */
	struct agenda_event *v = (struct agenda_event *)stored, e;
	v[0] = ev(2026, 1, 1, 0, 0, "Stored");
	v[1] = ev(2026, 1, 2, 0, 0, "");
	memset(v[1].title, 'x', AGENDA_TITLE);
	stored_len = (int)(2 * sizeof e);
	CHECK(agenda_count() == 2 && !strcmp(title(0), "Stored") && strlen(title(1)) == AGENDA_TITLE - 1);
	agenda_delete(1);
	agenda_delete(0);
	CHECK(agenda_count() == 0 && !agenda_get(0) && stored_len == 0);
	kv_writes = 0;
	uint32_t s0 = agenda_serial();

	/* Kept in time order, whatever order they come in. */
	e = ev(2026, 11, 12, 600, 660, "Standup");
	CHECK(agenda_set(AGENDA_EVENTS, &e) == 0);
	e = ev(2026, 11, 12, 480, 540, "Run");
	CHECK(agenda_set(AGENDA_EVENTS, &e) == 0);
	e = ev(2026, 11, 13, 0, 30, "Tomorrow");
	CHECK(agenda_set(AGENDA_EVENTS, &e) == 2);
	e = ev(2026, 10, 31, 1200, 1260, "Before");
	CHECK(agenda_set(AGENDA_EVENTS, &e) == 0);
	CHECK(agenda_count() == 4 && kv_writes == 4 && agenda_serial() != s0);
	CHECK(!strcmp(title(0), "Before") && !strcmp(title(1), "Run") && !strcmp(title(2), "Standup") && !strcmp(title(3), "Tomorrow"));

	/* A day's events and the first of them. */
	unsigned first = 99;
	CHECK(agenda_day(2026, 11, 12, &first) == 2 && first == 1);
	CHECK(agenda_day(2026, 11, 13, &first) == 1 && first == 3);
	CHECK(agenda_day(2026, 11, 14, &first) == 0 && first == 4);

	/* An edit takes the event out and puts it back in its place. */
	e = ev(2026, 11, 13, 720, 780, "Run");
	CHECK(agenda_set(1, &e) == 3);
	CHECK(!strcmp(title(1), "Standup") && !strcmp(title(2), "Tomorrow") && !strcmp(title(3), "Run"));
	CHECK(agenda_count() == 4);
	/* The same start goes by its end, then after the ones alike. */
	e = ev(2026, 11, 13, 0, 10, "Short");
	CHECK(agenda_set(AGENDA_EVENTS, &e) == 2);
	e = ev(2026, 11, 13, 0, 10, "Short too");
	CHECK(agenda_set(AGENDA_EVENTS, &e) == 3);
	agenda_delete(3);
	agenda_delete(40);
	CHECK(agenda_count() == 5 && !strcmp(title(3), "Tomorrow"));

	/* What is not a date or a time of day is refused and stores nothing. */
	unsigned w = kv_writes;
	struct agenda_event bad[] = {
		ev(2026, 2, 29, 0, 0, "x"), ev(2026, 13, 1, 0, 0, "x"), ev(2026, 4, 31, 0, 0, "x"),
		ev(1999, 1, 1, 0, 0, "x"), ev(2100, 1, 1, 0, 0, "x"), ev(2026, 1, 0, 0, 0, "x"),
		ev(2026, 1, 1, 1440, 0, "x"), ev(2026, 1, 1, 0, 1440, "x"),
	};
	for (unsigned i = 0; i < ARRAY_SIZE(bad); i++)
		CHECK(agenda_set(AGENDA_EVENTS, &bad[i]) < 0);
	CHECK(kv_writes == w && agenda_count() == 5);
	e = ev(2028, 2, 29, 0, 0, "Leap day");
	CHECK(agenda_set(AGENDA_EVENTS, &e) == 5);
	e = ev(2000, 2, 29, 23 * 60 + 30, 30, "Past midnight");
	CHECK(agenda_set(AGENDA_EVENTS, &e) == 0);

	/* Titles: an empty one is untitled, a long one cut short of a split
	 * character. */
	e = ev(2030, 1, 1, 0, 0, "");
	CHECK(agenda_set(AGENDA_EVENTS, &e) == 7 && !strcmp(title(7), AGENDA_UNTITLED));
	e = ev(2030, 1, 2, 0, 0, "");
	memcpy(e.title, "abcdefghijklmnopqrstuvwx", AGENDA_TITLE);
	CHECK(agenda_set(AGENDA_EVENTS, &e) == 8 && !strcmp(title(8), "abcdefghijklmnopqrstuvw"));
	e = ev(2030, 1, 3, 0, 0, "");
	memcpy(e.title, "abcdefghijklmnopqrstuv\xc3\xa9", AGENDA_TITLE);
	CHECK(agenda_set(AGENDA_EVENTS, &e) == 9 && !strcmp(title(9), "abcdefghijklmnopqrstuv"));

	/* Full: no more added, but an edit still goes. */
	while (agenda_count() < AGENDA_EVENTS) {
		e = ev(2031, 1, 1, agenda_count(), agenda_count(), "Filler");
		CHECK(agenda_set(AGENDA_EVENTS, &e) >= 0);
	}
	CHECK(stored_len == (int)(AGENDA_EVENTS * sizeof e));
	e = ev(2032, 1, 1, 0, 0, "One more");
	CHECK(agenda_set(AGENDA_EVENTS, &e) < 0 && agenda_count() == AGENDA_EVENTS);
	CHECK(agenda_set(0, &e) == AGENDA_EVENTS - 1 && !strcmp(title(AGENDA_EVENTS - 1), "One more"));

	/* The console's form. */
	CHECK(agenda_parse("2026-11-12 09:30 10:15 Dentist at noon", &e));
	CHECK(e.year == 2026 && e.month == 11 && e.day == 12 && e.start == 570 && e.end == 615 && !strcmp(e.title, "Dentist at noon"));
	CHECK(agenda_parse("2026-11-12 09:30 10:15", &e) && !strcmp(e.title, AGENDA_UNTITLED));
	CHECK(agenda_parse("2026-11-12  23:00   01:00  Late", &e) && e.start == 1380 && e.end == 60 && !strcmp(e.title, "Late"));
	CHECK(!agenda_parse("2026-11-31 09:30 10:15 x", &e));
	CHECK(!agenda_parse("2026-11-12 24:00 10:15 x", &e));
	CHECK(!agenda_parse("2026-11-12 09:60 10:15 x", &e));
	CHECK(!agenda_parse("2026-11-12 9:30 10:15 x", &e));
	CHECK(!agenda_parse("2026-11-12 09:30 10:15x", &e));
	CHECK(!agenda_parse("26-11-12 09:30 10:15", &e));
	CHECK(!agenda_parse("", &e));

	return stub_failures != 0 ? (fprintf(stderr, "agenda_test: %d failures\n", stub_failures), 1) : (printf("agenda_test: ok\n"), 0);
}
