#include <string.h>
#include "wcnss_msg.h"

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t get16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

void wcnss_msg_header(uint8_t *msg, uint32_t type, uint32_t len)
{
	put32(msg, type);
	put32(msg + 4, len);
}

bool wcnss_msg_parse(const uint8_t *msg, uint32_t n, uint32_t *type, uint32_t *len)
{
	if (n < WCNSS_HEADER)
		return false;
	*type = get32(msg);
	*len = get32(msg + 4);
	return true;
}

uint32_t wcnss_nv_frag(uint8_t *msg, uint16_t num, uint16_t flags, uint32_t size)
{
	uint32_t len = WCNSS_NV_HEADER + size;
	wcnss_msg_header(msg, WCNSS_NV_REQ, len);
	put16(msg + 8, num);
	put16(msg + 10, flags);
	put32(msg + 12, size);
	return len;
}

uint32_t wcnss_cal_frag(uint8_t *msg, uint32_t total, uint16_t num, uint16_t flags, uint32_t size)
{
	uint32_t len = WCNSS_CAL_HEADER + size;
	wcnss_msg_header(msg, WCNSS_CAL_DOWNLOAD_REQ, len);
	put32(msg + 8, total);
	put16(msg + 12, num);
	put16(msg + 14, flags);
	put32(msg + 16, size);
	return len;
}

static int drop(struct wcnss_cal *c)
{
	c->have = 0;
	c->next = 0;
	c->done = false;
	return -1;
}

int wcnss_cal_take(struct wcnss_cal *c, const uint8_t *msg, uint32_t n)
{
	if (n < WCNSS_CAL_HEADER)
		return drop(c);
	uint32_t total = get32(msg + 8), size = get32(msg + 16);
	uint16_t num = get16(msg + 12), flags = get16(msg + 14);
	if (n - WCNSS_CAL_HEADER < size || size > WCNSS_MAX_MSG)
		return drop(c);
	if (num == 0) {
		drop(c);
		if (total > c->max)
			return -1;
	} else if (num != c->next) {
		return drop(c);
	}
	if (c->have + size > c->max)
		return drop(c);
	memcpy(c->buf + c->have, msg + WCNSS_CAL_HEADER, size);
	c->have += size;
	c->next++;
	if (!(flags & WCNSS_FRAG_LAST))
		return 0;
	c->next = 0;
	c->done = true;
	return 1;
}
