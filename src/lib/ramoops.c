#include "lib/ramoops.h"

int32_t ramoops_console_offset(const struct ramoops_layout *l)
{
	uint32_t other = l->console_size + l->ftrace_size;
	if (!l->console_size || l->size < other)
		return -1;
	if (!l->record_size)
		return 0;
	uint32_t n = (l->size - other) / l->record_size;
	return n ? (int32_t)(n * l->record_size) : -1;
}

static void put32(volatile uint8_t *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

void ramoops_zone_fill(volatile uint8_t *zone, uint32_t zone_size, const struct ramoops_piece *p, unsigned np)
{
	if (zone_size <= RAMOOPS_HEADER)
		return;
	uint32_t total = 0, n = 0;
	for (unsigned k = 0; k < np; k++)
		total += p[k].n;
	uint32_t cap = zone_size - RAMOOPS_HEADER, skip = total > cap ? total - cap : 0;
	volatile uint8_t *data = zone + RAMOOPS_HEADER;
	for (unsigned k = 0; k < np; k++) {
		uint32_t from = skip < p[k].n ? skip : p[k].n;
		skip -= from;
		for (uint32_t i = from; i < p[k].n; i++)
			data[n++] = p[k].data[i];
	}
	put32(zone + 4, 0);
	put32(zone + 8, n);
	put32(zone, RAMOOPS_SIG);
}
