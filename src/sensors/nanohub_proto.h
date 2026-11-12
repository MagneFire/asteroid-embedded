/* The framing of the TicWatch Pro's sensor hub link, no hardware
 * involved. A packet is the sync byte 0x31, a sequence number, a reason
 * (the command id, or ACK/NAK/BUSY in an answer), a length byte, the
 * payload and a CRC-32 of all of it, every number little endian. The
 * CRC runs over 32-bit words, a last short word padded with zeros. A
 * READ answer is an event: a sensor sample, a log line, or an app
 * message, one of which carries the fuel gauge. */
#ifndef NANOHUB_PROTO_H
#define NANOHUB_PROTO_H
#include <stdbool.h>
#include <stdint.h>

#define NANOHUB_SYNC		0x31
#define NANOHUB_HEADER		10
#define NANOHUB_PAYLOAD_MAX	255
#define NANOHUB_PACKET_MAX	(NANOHUB_HEADER + NANOHUB_PAYLOAD_MAX + 4)

#define NANOHUB_ACK		0x0000
#define NANOHUB_NAK		0x0001
#define NANOHUB_BUSY		0x0002
#define NANOHUB_GET_VERSIONS	0x1000
#define NANOHUB_CLR_GET_INTR	0x1080
#define NANOHUB_MASK_INTR	0x1081
#define NANOHUB_UNMASK_INTR	0x1082
#define NANOHUB_READ		0x1090
#define NANOHUB_WRITE		0x1091

#define NANOHUB_ACK_INTR_SIZE	32

#define NANOHUB_EVT_CONFIG	0x300
#define NANOHUB_EVT_SENSOR	0x200
#define NANOHUB_EVT_APP		0x401

#define NANOHUB_CMD_DISABLE	0
#define NANOHUB_CMD_ENABLE	1
#define NANOHUB_CMD_FLUSH	2
#define NANOHUB_CMD_CFG_DATA	3
#define NANOHUB_RATE_ON_DEMAND	0xffffff00u
#define NANOHUB_RATE_ON_CHANGE	0xffffff01u
#define NANOHUB_RATE_ONE_SHOT	0xffffff02u
#define NANOHUB_HZ(hz)		((uint32_t)(hz) * 1024u)

#define NANOHUB_SENSOR_ACCEL		1
#define NANOHUB_SENSOR_HEART_RATE	0x10
#define NANOHUB_SENSOR_STEP_COUNTER	0x16
#define NANOHUB_SENSOR_FUEL_GAUGE	68
#define NANOHUB_SENSOR_HAND_UP		0x4b
#define NANOHUB_SENSOR_TIME		0x55

/* The fuel gauge report is 56 bytes: the event, the app id, 43 bytes of
 * app data of which 40 are the gauge's register cache. */
#define NANOHUB_FUEL_GAUGE_SIZE	56

enum { NANOHUB_OK, NANOHUB_NEED_MORE, NANOHUB_BAD };

struct nanohub_packet {
	uint32_t seq, reason;
	uint8_t len;
	const uint8_t *payload;		/* in the buffer decoded from */
};

enum nanohub_event_kind {
	NANOHUB_EV_EMPTY,		/* the hub has nothing pending */
	NANOHUB_EV_BAD,			/* too short to be an event */
	NANOHUB_EV_SENSOR,
	NANOHUB_EV_FUEL_GAUGE,
	NANOHUB_EV_APP,			/* any other message from an app */
	NANOHUB_EV_LOG,
	NANOHUB_EV_OTHER,
};

struct nanohub_fuel_gauge {
	uint8_t status, present;
	uint16_t control, flags;
	int16_t temp_dc;		/* 0.1 degC */
	uint16_t mv;
	uint16_t remaining, full;
	int16_t current_ma;		/* negative while discharging */
	uint16_t percent;
	int16_t power_mw;
	uint16_t health, design_mah;
	bool discharging, full_charge;	/* from the flags register */
};

struct nanohub_event {
	enum nanohub_event_kind kind;
	uint32_t type;			/* the event id, the top bit cleared */
	struct {
		uint8_t type;		/* the hub's sensor type */
		bool header;		/* the event has a sample header, else it is only a signal */
		bool wake;		/* from a wake-up sensor */
		uint8_t samples, flushes;
		uint64_t ref_ns;
		const uint8_t *points;	/* the first sample's header word onwards */
		unsigned size;
	} sensor;
	struct {
		uint64_t id;
		const uint8_t *data;
		unsigned size;
	} app;
	struct {
		char level;
		const uint8_t *text;
		unsigned size;
	} log;
	struct nanohub_fuel_gauge gauge;
};

uint32_t nanohub_crc32(const uint8_t *p, unsigned n);
/* Writes the packet to out (NANOHUB_PACKET_MAX), returns its size, 0 if len is over 255. */
unsigned nanohub_encode(uint8_t *out, uint32_t seq, uint32_t reason, const uint8_t *payload, unsigned len);
/* Finds a packet after any 0xff or 0x00 filler. *used is how many bytes to drop from buf:
 * all of it up to the packet's end on OK, up to the offending byte on BAD (not a sync byte,
 * or a CRC mismatch), none on NEED_MORE. */
int nanohub_decode(const uint8_t *buf, unsigned n, struct nanohub_packet *p, unsigned *used);
/* The interrupt word of an ACK (bit 0 wake complete, 1 wake data, 2 data, 3 command wait), 0 for any other packet. */
uint32_t nanohub_ack_intr(const struct nanohub_packet *p);
/* The payload of a READ, 8 bytes: the AP's boot time, so the hub stamps events in it. */
unsigned nanohub_read_payload(uint8_t *out, uint64_t boot_ns);
/* The payload of a WRITE that configures a sensor, 20 bytes and the cfg data after; 0 if over 255. */
unsigned nanohub_sensor_config(uint8_t *out, uint8_t type, uint8_t cmd, uint32_t rate, uint64_t latency_ns,
			       const uint8_t *data, unsigned dlen);
/* The payload of a WRITE that sets the hub's clock: 36 bytes. */
unsigned nanohub_time_config(uint8_t *out, uint64_t utc, int32_t gmtoff, bool h24);
/* Parses the payload of a READ answer; false for one that is bad. */
bool nanohub_event(const uint8_t *p, unsigned n, struct nanohub_event *ev);
/* Sample i of a sensor event of data_size bytes per sample: its time (the reference plus the deltas up to it) and data. */
bool nanohub_sample(const struct nanohub_event *ev, unsigned i, unsigned data_size, uint64_t *ns, const uint8_t **data);

#endif
