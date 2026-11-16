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
#endif
