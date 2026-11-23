/* Messages on the WCNSS control channel: a type and the whole length,
 * then the body. The NV file and calibration data go in fragments of
 * at most 3 KiB, so that a fragment fits in an SMD packet. No hardware
 * involved. */
#ifndef WCNSS_MSG_H
#define WCNSS_MSG_H
#include <stdbool.h>
#include <stdint.h>

#define WCNSS_VERSION_REQ	0x01000000u
#define WCNSS_VERSION_RSP	0x01000001u
#define WCNSS_NV_REQ		0x01000002u
#define WCNSS_NV_RSP		0x01000003u
#define WCNSS_CAL_UPLOAD_REQ	0x01000004u
#define WCNSS_CAL_UPLOAD_RSP	0x01000005u
#define WCNSS_CAL_DOWNLOAD_REQ	0x01000006u
#define WCNSS_CAL_DOWNLOAD_RSP	0x01000007u
#define WCNSS_BUILD_VER_REQ	0x01000009u
#define WCNSS_BUILD_VER_RSP	0x0100000au
#define WCNSS_CBC_COMPLETE_IND	0x0100000cu

#define WCNSS_HEADER		8
#define WCNSS_NV_HEADER		(WCNSS_HEADER + 8)
#define WCNSS_CAL_HEADER	(WCNSS_HEADER + 12)
#define WCNSS_FRAGMENT		3072
#define WCNSS_MAX_MSG		4096
/* The NV file starts with a validity bitmap the core does not take. */
#define WCNSS_NV_SKIP		4

#define WCNSS_FRAG_LAST		0x0001u
#define WCNSS_FRAG_MORE		0x0002u	/* another download follows */
#define WCNSS_FRAG_CAL_OK	0x8000u	/* we take calibration data back */

/* The status byte of the NV and calibration download replies. */
#define WCNSS_NV_DONE		1
#define WCNSS_NV_WAIT_CBC	2

/* The header of a message; the length is the whole message's. */
void wcnss_msg_header(uint8_t *msg, uint32_t type, uint32_t len);
bool wcnss_msg_parse(const uint8_t *msg, uint32_t n, uint32_t *type, uint32_t *len);
/* The headers in front of size bytes already at msg + WCNSS_NV_HEADER
 * or msg + WCNSS_CAL_HEADER; returns the message's length. */
uint32_t wcnss_nv_frag(uint8_t *msg, uint16_t num, uint16_t flags, uint32_t size);
uint32_t wcnss_cal_frag(uint8_t *msg, uint32_t total, uint16_t num, uint16_t flags, uint32_t size);

/* Calibration data the core uploads, fragment by fragment, to give
 * back to it on its next start. */
struct wcnss_cal {
	uint8_t *buf;
	uint32_t max, have;
	uint16_t next;
	bool done;
};

/* One whole upload message: 1 once its last fragment is in, 0 while
 * more are to come, < 0 for one out of order or too big, which drops
 * what was there. */
int wcnss_cal_take(struct wcnss_cal *c, const uint8_t *msg, uint32_t n);
#endif
