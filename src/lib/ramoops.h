/* Zones of a Linux pstore "ramoops" region, no hardware involved: a zone
 * is a {signature, start, size} header and its data. */
#ifndef RAMOOPS_H
#define RAMOOPS_H
#include <stdint.h>

#define RAMOOPS_SIG		0x43474244u	/* "DBGC" */
#define RAMOOPS_HEADER		12u

/* A kernel's region as its platform data gives it. */
struct ramoops_layout {
	uint32_t phys, size, record_size, console_size, ftrace_size;
};

struct ramoops_piece {
	const volatile uint8_t *data;
	uint32_t n;
};

/* Where the console zone starts in the region: after as many dump
 * records as fit before the console and ftrace zones, as the kernel lays
 * them out, at the start without dump records; -1 if the kernel would
 * have no console. */
int32_t ramoops_console_offset(const struct ramoops_layout *l);
/* Fill a zone with the pieces in order, keeping their end if they do not
 * all fit, as one the kernel reads in order. */
void ramoops_zone_fill(volatile uint8_t *zone, uint32_t zone_size, const struct ramoops_piece *p, unsigned np);
#endif
