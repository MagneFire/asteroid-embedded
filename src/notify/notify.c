/* Phone notifications as the AsteroidOS notification service receives
 * them: XML-like <insert> and <removed> messages, split over as many
 * characteristic writes as the MTU needs, so writes are collected until a
 * message ends. An insert with a known id replaces that notification.
 * A snoozed notification is put aside until its time (RTC seconds) and
 * then comes back as new, unless the phone removed or replaced it
 * meanwhile. */
#include <string.h>
#include "asteroid.h"
#include "notify.h"

static char msg[2048];
static unsigned msg_len;
static struct notification list[NOTIFY_MAX], aside[NOTIFY_MAX];
static uint32_t aside_until[NOTIFY_MAX];
static unsigned count, asides;
static uint32_t serial;

static const char *find_tag(const char *s, const char *end, const char *tag, const char **body_end)
{
	char open[16], close[16];
	size_t tl = strlen(tag);
	if (tl + 3 > sizeof open)
		return NULL;
	open[0] = '<';
	memcpy(open + 1, tag, tl);
	open[tl + 1] = '>';
	open[tl + 2] = 0;
	close[0] = '<';
	close[1] = '/';
	memcpy(close + 2, tag, tl);
	close[tl + 2] = '>';
	close[tl + 3] = 0;
	size_t ol = tl + 2, cl = tl + 3;
	for (const char *p = s; p + ol <= end; p++) {
		if (memcmp(p, open, ol))
			continue;
		const char *b = p + ol;
		for (const char *q = b; q + cl <= end; q++)
			if (!memcmp(q, close, cl)) {
				*body_end = q;
				return b;
			}
		return NULL;
	}
	return NULL;
}

static unsigned utf8_cut(const char *s, unsigned n)
{
	unsigned start = n;
	while (start && ((unsigned char)s[start - 1] & 0xc0) == 0x80)
		start--;
	if (!start || !((unsigned char)s[start - 1] & 0x80))
		return n;
	unsigned char lead = (unsigned char)s[start - 1];
	unsigned need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
	return n - (start - 1) == need ? n : start - 1;
}

static void field(const char *s, const char *end, const char *tag, char *out, unsigned size)
{
	static const struct {
		const char *name;
		char c;
	} entities[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
	const char *e, *b = find_tag(s, end, tag, &e);
	unsigned n = 0;
	for (; b && b < e && n < size - 1; b++) {
		char c = *b;
		if (c == '&')
			for (unsigned i = 0; i < ARRAY_SIZE(entities); i++) {
				size_t l = strlen(entities[i].name);
				if ((size_t)(e - b) >= l && !memcmp(b, entities[i].name, l)) {
					c = entities[i].c;
					b += l - 1;
					break;
				}
			}
		out[n++] = c;
	}
	if (b && b < e)
		n = utf8_cut(out, n);
	out[n] = 0;
}

static int find_id(int32_t id)
{
	for (unsigned i = 0; i < count; i++)
		if (list[i].id == id)
			return (int)i;
	return -1;
}

static void remove_at(unsigned i)
{
	memmove(&list[i], &list[i + 1], (count - i - 1) * sizeof list[0]);
	count--;
	serial++;
}

static void unsnooze_at(unsigned i)
{
	memmove(&aside[i], &aside[i + 1], (asides - i - 1) * sizeof aside[0]);
	memmove(&aside_until[i], &aside_until[i + 1], (asides - i - 1) * sizeof aside_until[0]);
	asides--;
}

static void drop_snoozed(int32_t id)
{
	for (unsigned i = asides; i--;)
		if (aside[i].id == id)
			unsnooze_at(i);
}

static struct notification *add(void)
{
	if (count == NOTIFY_MAX)
		remove_at(0);
	return &list[count++];
}

static void handle(const char *s, const char *end)
{
	char id_text[16];
	field(s, end, "id", id_text, sizeof id_text);
	int32_t id = id_text[0] == '-' ? -(int32_t)strtoul(id_text + 1, NULL, 10) : (int32_t)strtoul(id_text, NULL, 10);
	const char *e;
	if (find_tag(s, end, "insert", &e)) {
		int i = find_id(id);
		if (i >= 0)
			remove_at((unsigned)i);
		drop_snoozed(id);
		struct notification *n = add();
		memset(n, 0, sizeof *n);
		n->id = id;
		field(s, end, "pn", n->package, sizeof n->package);
		field(s, end, "an", n->app, sizeof n->app);
		field(s, end, "ai", n->icon, sizeof n->icon);
		field(s, end, "su", n->summary, sizeof n->summary);
		field(s, end, "bo", n->body, sizeof n->body);
		field(s, end, "vb", n->vibrate, sizeof n->vibrate);
		n->time = pmic_rtc_read();
		n->seq = ++serial;
		log_printf("notify: %ld from %s: %s\n", (long)id, n->app, n->summary);
	} else if (find_tag(s, end, "removed", &e)) {
		int i = find_id(id);
		if (i >= 0)
			remove_at((unsigned)i);
		drop_snoozed(id);
		log_printf("notify: %ld removed%s\n", (long)id, i < 0 ? " (unknown)" : "");
	}
}

void notify_feed(const uint8_t *p, unsigned n)
{
	if (msg_len + n > sizeof msg) {
		log_printf("notify: message over %u bytes dropped\n", (unsigned)sizeof msg);
		msg_len = 0;
		return;
	}
	memcpy(msg + msg_len, p, n);
	msg_len += n;
	static const char *const ends[] = { "</insert>", "</removed>" };
	for (unsigned i = 0; i < ARRAY_SIZE(ends); i++) {
		size_t l = strlen(ends[i]);
		if (msg_len >= l && !memcmp(msg + msg_len - l, ends[i], l)) {
			handle(msg, msg + msg_len);
			msg_len = 0;
			return;
		}
	}
}

unsigned notify_count(void)
{
	return count;
}

const struct notification *notify_get(unsigned i)
{
	return i < count ? &list[i] : NULL;
}

void notify_dismiss(int32_t id)
{
	int i = find_id(id);
	if (i >= 0)
		remove_at((unsigned)i);
}

void notify_dismiss_all(void)
{
	if (count) {
		count = 0;
		serial++;
	}
}

void notify_snooze(int32_t id, uint32_t until)
{
	int i = find_id(id);
	if (i < 0)
		return;
	if (asides == NOTIFY_MAX)
		unsnooze_at(0);
	aside[asides] = list[i];
	aside_until[asides++] = until;
	remove_at((unsigned)i);
	log_printf("notify: %ld snoozed\n", (long)id);
}

void notify_tick(uint32_t now)
{
	for (unsigned i = 0; i < asides;) {
		if ((int32_t)(now - aside_until[i]) < 0) {
			i++;
			continue;
		}
		struct notification *n = add();
		*n = aside[i];
		n->time = now;
		n->seq = ++serial;
		log_printf("notify: %ld back from snooze\n", (long)n->id);
		unsnooze_at(i);
	}
}

uint32_t notify_serial(void)
{
	return serial;
}
