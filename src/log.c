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

static void console_put(char c)
{
	volatile struct zone *z = CONSOLE;
	z->data[z->start] = (uint8_t)c;
	z->start = (z->start + 1) % CONSOLE_CAP;
	if (z->size < CONSOLE_CAP)
		z->size++;
}

void log_init(void)
{
	volatile struct zone *z = CONSOLE;
	if (z->sig != SIG || z->start >= CONSOLE_CAP || z->size > CONSOLE_CAP) {
		z->sig = SIG;
		z->start = 0;
		z->size = 0;
	}
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

static void log_write(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		console_put(s[i]);
	if (sink)
		sink(s, n);
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

const char *log_previous_record(void)
{
	return have_previous ? previous : NULL;
}
