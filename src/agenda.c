/* The events are kept sorted by date, start and end, so a day's events
 * are next to each other, and stored as they are in memory, the count
 * following from the length. */
#include <string.h>
#include "asteroid.h"
#include "agenda.h"
#include "storage/storage.h"

#define KV_AGENDA	"agenda"
#define DAY_MINUTES	1440

_Static_assert(sizeof(struct agenda_event) == 32 && AGENDA_EVENTS * sizeof(struct agenda_event) <= KV_VALUE_MAX,
	       "agenda record");

static struct agenda_event events[AGENDA_EVENTS];
static unsigned count;
static bool loaded;
static uint32_t serial;

static void load(void)
{
	if (loaded)
		return;
	int n = kv_get(KV_AGENDA, events, sizeof events);
	/* Without a store yet, try again next time. */
	if (n == -1)
		return;
	count = n > 0 && n <= (int)sizeof events && n % sizeof events[0] == 0 ? (unsigned)n / sizeof events[0] : 0;
	for (unsigned i = 0; i < count; i++)
		events[i].title[AGENDA_TITLE - 1] = 0;
	loaded = true;
}

static void save(void)
{
	serial++;
	kv_set(KV_AGENDA, events, count * sizeof events[0]);
}

static unsigned month_days(uint32_t year, uint32_t month)
{
	static const uint8_t days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	return month == 2 && leap ? 29 : days[month - 1];
}

static bool valid(const struct agenda_event *e)
{
	return e->year >= 2000 && e->year <= 2099 && e->month >= 1 && e->month <= 12 && e->day >= 1 &&
	       e->day <= month_days(e->year, e->month) && e->start < DAY_MINUTES && e->end < DAY_MINUTES;
}

static bool before(const struct agenda_event *a, const struct agenda_event *b)
{
	uint32_t ka = (uint32_t)a->year << 9 | a->month << 5 | a->day, kb = (uint32_t)b->year << 9 | b->month << 5 | b->day;
	if (ka != kb)
		return ka < kb;
	return a->start != b->start ? a->start < b->start : a->end < b->end;
}

/* At most AGENDA_TITLE - 1 bytes, not cutting a UTF-8 character. */
static void set_title(char *out, const char *s)
{
	size_t n = strnlen(s, AGENDA_TITLE);
	if (n > AGENDA_TITLE - 1) {
		n = AGENDA_TITLE - 1;
		while (n && ((uint8_t)s[n] & 0xc0) == 0x80)
			n--;
	}
	if (!n) {
		s = AGENDA_UNTITLED;
		n = strlen(s);
	}
	memcpy(out, s, n);
	memset(out + n, 0, AGENDA_TITLE - n);
}

unsigned agenda_count(void)
{
	load();
	return count;
}

const struct agenda_event *agenda_get(unsigned i)
{
	load();
	return i < count ? &events[i] : NULL;
}

int agenda_set(unsigned i, const struct agenda_event *e)
{
	load();
	if (!valid(e) || (i >= count && count == AGENDA_EVENTS))
		return -1;
	struct agenda_event v = *e;
	char title[AGENDA_TITLE + 1];
	memcpy(title, e->title, AGENDA_TITLE);
	title[AGENDA_TITLE] = 0;
	set_title(v.title, title);
	if (i < count)
		memmove(&events[i], &events[i + 1], (--count - i) * sizeof events[0]);
	unsigned at = 0;
	while (at < count && !before(&v, &events[at]))
		at++;
	memmove(&events[at + 1], &events[at], (count - at) * sizeof events[0]);
	events[at] = v;
	count++;
	save();
	return (int)at;
}

void agenda_delete(unsigned i)
{
	load();
	if (i >= count)
		return;
	memmove(&events[i], &events[i + 1], (count - i - 1) * sizeof events[0]);
	count--;
	save();
}

unsigned agenda_day(uint32_t year, uint32_t month, uint32_t day, unsigned *first)
{
	load();
	unsigned i = 0, n = 0;
	while (i < count && (events[i].year != year || events[i].month != month || events[i].day != day))
		i++;
	while (i + n < count && events[i + n].year == year && events[i + n].month == month && events[i + n].day == day)
		n++;
	if (first)
		*first = i;
	return n;
}

static bool number(const char **p, unsigned digits, char after, uint32_t *v)
{
	*v = 0;
	for (unsigned i = 0; i < digits; i++, (*p)++) {
		if (**p < '0' || **p > '9')
			return false;
		*v = *v * 10 + (uint32_t)(**p - '0');
	}
	return after ? *(*p)++ == after : true;
}

static bool clock_time(const char **p, char after, uint16_t *minutes)
{
	uint32_t h, m;
	if (!number(p, 2, ':', &h) || !number(p, 2, after, &m) || h > 23 || m > 59)
		return false;
	*minutes = (uint16_t)(h * 60 + m);
	return true;
}

bool agenda_parse(const char *s, struct agenda_event *e)
{
	uint32_t y, mo, d;
	memset(e, 0, sizeof *e);
	if (!number(&s, 4, '-', &y) || !number(&s, 2, '-', &mo) || !number(&s, 2, ' ', &d))
		return false;
	while (*s == ' ')
		s++;
	if (!clock_time(&s, ' ', &e->start))
		return false;
	while (*s == ' ')
		s++;
	if (!clock_time(&s, 0, &e->end) || (*s && *s != ' '))
		return false;
	while (*s == ' ')
		s++;
	e->year = (uint16_t)y;
	e->month = (uint8_t)(mo <= 12 ? mo : 0);
	e->day = (uint8_t)(d <= 31 ? d : 0);
	set_title(e->title, s);
	return valid(e);
}

uint32_t agenda_serial(void) { return serial; }
