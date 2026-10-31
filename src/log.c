/* Persistent log in DRAM that survives a warm reset, laid out as a pstore
 * "ramoops" region (dump records followed by a console ring, each zone a
 * {signature, start, size} header and data) so the bootloader can stage it
 * for `fastboot get_staged`. The console ring takes every log line; a
 * fault writes one record for the next boot to report. */
#include <string.h>
#include "asteroid.h"

#define SIG 0x43474244u		/* "DBGC" */

struct zone {
	uint32_t sig, start, size;
	uint8_t data[];
};

#define DUMP_ZONES	((RAMOOPS_SIZE - RAMOOPS_CONSOLE_SIZE) / RAMOOPS_RECORD_SIZE)
#define RECORD		((volatile struct zone *)(uintptr_t)RAMOOPS_BASE)
#define CONSOLE		((volatile struct zone *)(uintptr_t)(RAMOOPS_BASE + DUMP_ZONES * RAMOOPS_RECORD_SIZE))
#define CONSOLE_CAP	(RAMOOPS_CONSOLE_SIZE - sizeof(struct zone))
#define RECORD_CAP	(RAMOOPS_RECORD_SIZE - sizeof(struct zone))

static char previous[RECORD_CAP + 1];
static bool have_previous;
static void (*sink)(const char *s, size_t n);
/* Bytes put in the console ring since boot, counting those it held then:
 * the positions log_read works with, which keep their meaning while the
 * ring wraps. */
static uint32_t written;

static void console_put(char c)
{
	volatile struct zone *z = CONSOLE;
	z->data[z->start] = (uint8_t)c;
	z->start = (z->start + 1) % CONSOLE_CAP;
	if (z->size < CONSOLE_CAP)
		z->size++;
	written++;
}

void log_init(void)
{
	volatile struct zone *z = CONSOLE;
	if (z->sig != SIG || z->start >= CONSOLE_CAP || z->size > CONSOLE_CAP) {
		z->sig = SIG;
		z->start = 0;
		z->size = 0;
	}
	written = z->size;
	volatile struct zone *r = RECORD;
	if (r->sig == SIG && r->size > 0 && r->size <= RECORD_CAP) {
		for (uint32_t i = 0; i < r->size; i++)
			previous[i] = (char)r->data[i];
		previous[r->size] = 0;
		have_previous = true;
	}
	r->sig = 0;
	r->start = 0;
	r->size = 0;
}

void log_set_sink(void (*fn)(const char *s, size_t n)) { sink = fn; }

/* Called from the main loop and from interrupt handlers: the ring
 * indices are updated with interrupts masked so a line from an interrupt
 * cannot tear one the main loop is writing. */
static void log_write(const char *s, size_t n)
{
	uint32_t flags = irq_save();
	for (size_t i = 0; i < n; i++)
		console_put(s[i]);
	if (sink)
		sink(s, n);
	irq_restore(flags);
}

void log_puts(const char *s) { log_write(s, strlen(s)); }

void log_printf(const char *fmt, ...)
{
	char buf[256];
	uint32_t ms = time_ms();
	int n = snprintf(buf, sizeof buf, "[%lu.%03lu] ", ms / 1000, ms % 1000);
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof buf - 1)
		n = (int)sizeof buf - 1;
	log_write(buf, (size_t)n);
}

/* A dump record in the kernel's "====<seconds>.<micros>-D\n" header
 * format, which the bootloader's dump command expects. */
void log_record(const char *text)
{
	volatile struct zone *r = RECORD;
	char hdr[32];
	uint32_t us = time_us();
	int n = snprintf(hdr, sizeof hdr, "====%10lu.%06lu-D\n", us / 1000000, us % 1000000);
	uint32_t len = 0;
	for (int i = 0; i < n && len < RECORD_CAP; i++)
		r->data[len++] = (uint8_t)hdr[i];
	for (const char *p = text; *p && len < RECORD_CAP; p++)
		r->data[len++] = (uint8_t)*p;
	r->start = 0;
	r->size = len;
	r->sig = SIG;
	dsb();
}

uint32_t log_end(void)
{
	return written;
}

/* Where the last `lines` lines start. */
uint32_t log_lines_from(unsigned lines)
{
	volatile struct zone *z = CONSOLE;
	uint32_t flags = irq_save();
	uint32_t size = z->size, base = (z->start + CONSOLE_CAP - size) % CONSOLE_CAP, end = written;
	irq_restore(flags);
	uint32_t from = 0;
	unsigned seen = 0;
	if (!lines)
		return end;
	for (uint32_t i = size; i-- > 0;) {
		if (!(i % 4096))
			wdt_feed();
		if (z->data[(base + i) % CONSOLE_CAP] == '\n' && i + 1 < size && ++seen == lines) {
			from = i + 1;
			break;
		}
	}
	return end - size + from;
}

/* Copies up to max bytes from *pos towards end and moves *pos on; what
 * was overwritten since *pos was taken is skipped. */
size_t log_read(uint32_t *pos, uint32_t end, char *buf, size_t max)
{
	volatile struct zone *z = CONSOLE;
	uint32_t flags = irq_save();
	uint32_t size = z->size, start = z->start, w = written;
	if (w - *pos > size)
		*pos = w - size;
	if (end - *pos > w - *pos)
		*pos = end;
	size_t n = 0;
	while (n < max && *pos != end && *pos != w) {
		buf[n++] = (char)z->data[(start + CONSOLE_CAP - (w - *pos)) % CONSOLE_CAP];
		(*pos)++;
	}
	irq_restore(flags);
	return n;
}

void log_grep(const char *pat, unsigned lines, void (*out)(const char *s, size_t n, void *ctx), void *ctx)
{
	volatile struct zone *z = CONSOLE;
	uint32_t size = z->size, base = (z->start + CONSOLE_CAP - size) % CONSOLE_CAP;
	static uint32_t starts[64];
	unsigned found = 0;
	size_t plen = strlen(pat);
	if (lines > 64)
		lines = 64;
	uint32_t end = size;
	uint32_t stop = size > 65536 ? size - 65536 : 0;
	for (uint32_t i = size; i-- > stop && found < lines;) {
		if (i && z->data[(base + i - 1) % CONSOLE_CAP] != '\n')
			continue;
		for (uint32_t j = i; j + plen <= end; j++) {
			size_t k = 0;
			while (k < plen && z->data[(base + j + k) % CONSOLE_CAP] == (uint8_t)pat[k])
				k++;
			if (k == plen) {
				starts[found++] = i;
				break;
			}
		}
		end = i;
	}
	char buf[160];
	while (found--) {
		size_t n = 0;
		for (uint32_t i = starts[found]; i < size && n < sizeof buf; i++) {
			buf[n++] = (char)z->data[(base + i) % CONSOLE_CAP];
			if (buf[n - 1] == '\n')
				break;
		}
		out(buf, n, ctx);
	}
}

const char *log_previous_record(void)
{
	return have_previous ? previous : NULL;
}
