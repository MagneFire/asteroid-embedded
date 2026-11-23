#include "lib/ramoops.h"

uint32_t ramoops_console_offset(const struct ramoops_layout *l)
{
	uint32_t other = l->console_size + l->ftrace_size;
	if (!l->record_size || l->size < other)
		return 0;
	uint32_t n = (l->size - other) / l->record_size;
	return n ? n * l->record_size : 0;
}

static void put32(volatile uint8_t *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

void ramoops_zone_fill(volatile uint8_t *zone, uint32_t zone_size, const volatile uint8_t *a, uint32_t an,
		       const volatile uint8_t *b, uint32_t bn)
{
	if (zone_size <= RAMOOPS_HEADER)
		return;
	uint32_t cap = zone_size - RAMOOPS_HEADER, skip = an + bn > cap ? an + bn - cap : 0, n = 0;
	volatile uint8_t *data = zone + RAMOOPS_HEADER;
	for (uint32_t i = skip; i < an; i++)
		data[n++] = a[i];
	for (uint32_t i = skip > an ? skip - an : 0; i < bn; i++)
		data[n++] = b[i];
	put32(zone + 4, 0);
	put32(zone + 8, n);
	put32(zone, RAMOOPS_SIG);
}
