/* WCNSS control messages: swift's NV file in fragments, calibration
 * data back from the core. */
#include <stdio.h>
#include <string.h>
#include "soc/qcom/wcnss_msg.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static uint8_t msg[WCNSS_MAX_MSG];

static uint32_t upload(uint32_t total, uint16_t num, uint16_t flags, uint32_t size, uint8_t fill)
{
	wcnss_msg_header(msg, WCNSS_CAL_UPLOAD_REQ, WCNSS_CAL_HEADER + size);
	memcpy(msg + 8, &total, 4);
	memcpy(msg + 12, &num, 2);
	memcpy(msg + 14, &flags, 2);
	memcpy(msg + 16, &size, 4);
	memset(msg + WCNSS_CAL_HEADER, fill, size);
	return WCNSS_CAL_HEADER + size;
}

int main(void)
{
	uint32_t type, len;
	CHECK(!wcnss_msg_parse(msg, 7, &type, &len));
	const uint8_t version[12] = { 1, 0, 0, 1, 12, 0, 0, 0, 1, 5, 1, 2 };
	CHECK(wcnss_msg_parse(version, 12, &type, &len) && type == WCNSS_VERSION_RSP && len == 12);

	/* 31723 bytes on swift: 31719 after the bitmap, 10 full fragments
	 * of 3088 bytes as the vendor logs them and 999 bytes last. */
	uint32_t blob = 31723 - WCNSS_NV_SKIP, frags = (blob + WCNSS_FRAGMENT - 1) / WCNSS_FRAGMENT;
	CHECK(frags == 11);
	CHECK(wcnss_nv_frag(msg, 2, 0, WCNSS_FRAGMENT) == 3088);
	CHECK(u32(msg) == WCNSS_NV_REQ && u32(msg + 4) == 3088 && u16(msg + 8) == 2 && u16(msg + 10) == 0 &&
	      u32(msg + 12) == WCNSS_FRAGMENT);
	uint32_t last = blob - 10 * WCNSS_FRAGMENT;
	CHECK(last == 999);
	CHECK(wcnss_nv_frag(msg, 10, WCNSS_FRAG_LAST | WCNSS_FRAG_CAL_OK, last) == WCNSS_NV_HEADER + 999);
	CHECK(u16(msg + 10) == 0x8001 && u32(msg + 12) == 999);

	CHECK(wcnss_cal_frag(msg, 5000, 1, WCNSS_FRAG_LAST | WCNSS_FRAG_MORE, 1928) == 20 + 1928);
	CHECK(u32(msg) == WCNSS_CAL_DOWNLOAD_REQ && u32(msg + 4) == 1948 && u32(msg + 8) == 5000 && u16(msg + 12) == 1 &&
	      u16(msg + 14) == 3 && u32(msg + 16) == 1928);

	static uint8_t store[8192];
	struct wcnss_cal c = { .buf = store, .max = sizeof store };
	CHECK(wcnss_cal_take(&c, msg, upload(5000, 0, 0, 3072, 0xaa)) == 0 && c.have == 3072 && !c.done);
	CHECK(wcnss_cal_take(&c, msg, upload(5000, 1, WCNSS_FRAG_LAST, 1928, 0xbb)) == 1);
	CHECK(c.done && c.have == 5000 && c.next == 0 && store[3071] == 0xaa && store[3072] == 0xbb);
	/* A new upload replaces it. */
	CHECK(wcnss_cal_take(&c, msg, upload(100, 0, WCNSS_FRAG_LAST, 100, 0xcc)) == 1 && c.have == 100 && store[0] == 0xcc);
	/* Out of order. */
	CHECK(wcnss_cal_take(&c, msg, upload(5000, 0, 0, 3072, 1)) == 0);
	CHECK(wcnss_cal_take(&c, msg, upload(5000, 2, WCNSS_FRAG_LAST, 1928, 2)) == -1 && !c.done && !c.have);
	/* Bigger than we keep, in total or by its fragments. */
	CHECK(wcnss_cal_take(&c, msg, upload(9000, 0, 0, 3072, 1)) == -1 && !c.have);
	CHECK(wcnss_cal_take(&c, msg, upload(8000, 0, 0, 3072, 1)) == 0);
	CHECK(wcnss_cal_take(&c, msg, upload(8000, 1, 0, 3072, 1)) == 0);
	CHECK(wcnss_cal_take(&c, msg, upload(8000, 2, WCNSS_FRAG_LAST, 3072, 1)) == -1 && !c.have);
	/* A fragment longer than its message, and a short message. */
	uint32_t n = upload(100, 0, WCNSS_FRAG_LAST, 100, 1);
	CHECK(wcnss_cal_take(&c, msg, n - 1) == -1);
	CHECK(wcnss_cal_take(&c, msg, 19) == -1);

	if (failures)
		printf("%d failures\n", failures);
	else
		printf("wcnss_msg: all passed\n");
	return failures != 0;
}
