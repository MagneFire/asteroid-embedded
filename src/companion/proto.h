/* The framework messages between beluga's application processor and
 * its Apollo3, no hardware involved: protobufs with varint and
 * length-delimited fields only, a field at 0 left out. A time is Unix
 * seconds (int32) and the zone's offset from UTC in minutes (sint32). */
#ifndef COMPANION_PROTO_H
#define COMPANION_PROTO_H
#include <stdbool.h>
#include <stdint.h>

enum proto_id {
	PROTO_BATTERY = 372,
	PROTO_NORMAL = 373,
	PROTO_POWER_OFF = 374,
	PROTO_WRISTBAND = 375,
	PROTO_MODE_REQUEST = 376,
	PROTO_REPLY = 377,
	PROTO_OOBE = 384,
	PROTO_BOOT = 386,
	PROTO_BOOT_ACK = 387,
};

/* Normal: the AP runs the watch. Wristband: the Apollo3 does, with the
 * AP on. Power off: the Apollo3 does, with the AP going down. */
enum proto_mode { PROTO_MODE_NORMAL, PROTO_MODE_WRISTBAND, PROTO_MODE_POWER_OFF };
enum proto_reboot { PROTO_REBOOT_DEBUG, PROTO_REBOOT_PIN, PROTO_REBOOT_FAULT, PROTO_REBOOT_OTHER };

struct proto_time {
	int32_t time, zone_min;
};

struct proto_reply {
	unsigned mode, last_mode, reboot;
	struct proto_time time;
};

uint32_t proto_zigzag(int32_t v);
int32_t proto_unzigzag(uint32_t v);

/* The mode a work mode message names, by its id; -1 for another id. */
int proto_mode_of(unsigned id);
/* 373, 374 and 375: the mode field and the time, 0 where absent; false
 * if anything in it is malformed. */
bool proto_work_mode(const uint8_t *p, unsigned n, unsigned *mode, struct proto_time *t);
/* 372: the capacity in percent of a battery preference, or -1 if it
 * has none. */
int proto_battery(const uint8_t *p, unsigned n);
/* 386: the time it carries. */
bool proto_boot(const uint8_t *p, unsigned n, struct proto_time *t);
/* 377: its length, which is 0 with every field at 0, or -1 if cap is
 * too small. */
int proto_reply_put(uint8_t *out, unsigned cap, const struct proto_reply *r);
/* The reboot reason for the Apollo3's reset status bits, and whether
 * the last run ended in a fault. */
unsigned proto_reboot_reason(uint32_t reset_status, bool faulted);
/* Whether the reset status bits say power-on or brown-out. */
bool proto_powered_up(uint32_t reset_status);

/* The sensor channel: a RequestBody from the AP's sensor HAL, with
 * fixed32 handles (the Android types) and fixed64 times in ns, and a
 * ResponseBody of events for it. */
enum proto_sns_handle {
	PROTO_SNS_META = 0,
	PROTO_SNS_ACCEL = 1,
	PROTO_SNS_MAG = 2,
	PROTO_SNS_GYRO = 4,
	PROTO_SNS_LIGHT = 5,
	PROTO_SNS_STEP_DETECTOR = 18,
	PROTO_SNS_STEP_COUNTER = 19,
	PROTO_SNS_HEART_RATE = 21,
	PROTO_SNS_WRIST_TILT = 26,
};
/* Android's SENSOR_STATUS_*. */
enum { PROTO_SNS_NO_CONTACT = -1, PROTO_SNS_UNRELIABLE, PROTO_SNS_LOW, PROTO_SNS_MEDIUM, PROTO_SNS_HIGH };
enum proto_sns_op { PROTO_SNS_FLUSH = 2, PROTO_SNS_ACTIVATE = 3, PROTO_SNS_BATCH = 7 };
#define PROTO_SNS_EVENTS	9
#define PROTO_SNS_EVENT_LEN	40u
#define PROTO_SNS_REPORT_MAX	(2 + PROTO_SNS_EVENTS * PROTO_SNS_EVENT_LEN)

/* time_ns is the AP's elapsedRealtimeNano at an activate. */
struct proto_sns_cmd {
	unsigned op;
	uint32_t handle;
	bool enable;
	uint64_t time_ns, period_ns, latency_ns;
};
/* False for something malformed or without a payload. */
bool proto_sns_cmd(const uint8_t *p, unsigned n, struct proto_sns_cmd *c);

/* The HAL copies the four words as they are: x, y and z as floats and
 * the accuracy, or a scalar in the first. A flush is completed by a
 * PROTO_SNS_META event with the handle in its first word. The heart
 * rate goes as the HAL's heart_rate_android_t instead: the bpm as a
 * float in the first word, the status (an int32) in the second. */
struct proto_sns_event {
	uint32_t handle;
	uint64_t time_ns;
	uint32_t word[4];
};
uint32_t proto_float_bits(float f);
/* Its length, 0 if cap is too small. */
unsigned proto_sns_report(uint8_t *out, unsigned cap, const struct proto_sns_event *e, unsigned n);
#endif
