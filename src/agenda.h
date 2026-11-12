/* The Agenda app's events: up to AGENDA_EVENTS of them in the key-value
 * store, in time order, each a date, a start and an end in minutes of
 * that day and a title. agenda_set replaces event i, or adds one for an
 * i past the end, and returns where it went in the order, -1 when it is
 * not a date and time or there is no room; an empty title becomes
 * AGENDA_UNTITLED. agenda_day counts the events on a day and gives the
 * first. agenda_parse reads "yyyy-mm-dd hh:mm hh:mm title", as the
 * console takes an event. agenda_serial changes with every change. */
#ifndef AGENDA_H
#define AGENDA_H
#include <stdint.h>
#include <stdbool.h>

#define AGENDA_EVENTS	32
#define AGENDA_TITLE	24
#define AGENDA_UNTITLED	"Untitled event"

struct agenda_event {
	uint16_t year;
	uint8_t month, day;
	uint16_t start, end;
	char title[AGENDA_TITLE];
};

unsigned agenda_count(void);
const struct agenda_event *agenda_get(unsigned i);
int agenda_set(unsigned i, const struct agenda_event *e);
void agenda_delete(unsigned i);
unsigned agenda_day(uint32_t year, uint32_t month, uint32_t day, unsigned *first);
bool agenda_parse(const char *s, struct agenda_event *e);
uint32_t agenda_serial(void);
#endif
