/* Calendar date from seconds since the Unix epoch and back (proleptic
 * Gregorian, Howard Hinnant's days-from-civil and its inverse by era). */
#include "asteroid.h"

void civil_from_epoch(uint32_t secs, struct civil *c)
{
	uint32_t days = secs / 86400, rem = secs % 86400;
	c->hour = rem / 3600;
	c->minute = rem % 3600 / 60;
	c->second = rem % 60;
	c->weekday = (days + 4) % 7;
	int32_t z = (int32_t)days + 719468;
	int32_t era = z / 146097;
	uint32_t doe = (uint32_t)(z - era * 146097);
	uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	uint32_t mp = (5 * doy + 2) / 153;
	c->day = doy - (153 * mp + 2) / 5 + 1;
	c->month = mp < 10 ? mp + 3 : mp - 9;
	c->year = yoe + (uint32_t)era * 400 + (c->month <= 2);
}

uint32_t epoch_from_civil(const struct civil *c)
{
	int32_t y = (int32_t)c->year - (c->month <= 2);
	int32_t era = (y >= 0 ? y : y - 399) / 400;
	uint32_t yoe = (uint32_t)(y - era * 400);
	uint32_t mp = c->month > 2 ? c->month - 3 : c->month + 9;
	uint32_t doy = (153 * mp + 2) / 5 + c->day - 1;
	uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	int32_t days = era * 146097 + (int32_t)doe - 719468;
	return (uint32_t)days * 86400 + c->hour * 3600 + c->minute * 60 + c->second;
}
