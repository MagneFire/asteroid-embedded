#include <string.h>
#include "nanohub_proto.h"

#define APP_ID_FUEL_GAUGE	((0x4d6f62766full << 24) | 4)
#define APP_MSG_CUSTOM		0xfe
#define EVT_LOG			0x3b474f4cu
#define EVT_LOG_NO_HAL		0x474f4c41u
#define CONFIG_SIZE		20
#define SENSOR_HEADER		12
#define APP_HEADER		13
#define TIME_SIZE		16
#define GAUGE_DATA_SIZE		43
#define GAUGE_CACHE		3
#define GAUGE_FLAG_DSG		(1u << 0)
#define GAUGE_FLAG_FC		(1u << 9)

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
	return get32(p) | (uint64_t)get32(p + 4) << 32;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static void put64(uint8_t *p, uint64_t v)
{
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t crc_word(uint32_t crc, uint32_t d)
{
	static const uint32_t t[16] = { 0x00000000, 0x04c11db7, 0x09823b6e, 0x0d4326d9, 0x130476dc, 0x17c56b6b,
					0x1a864db2, 0x1e475005, 0x2608edb8, 0x22c9f00f, 0x2f8ad6d6, 0x2b4bcb61,
					0x350c9b64, 0x31cd86d3, 0x3c8ea00a, 0x384fbdbd };
	crc ^= d;
	for (int i = 0; i < 8; i++)
		crc = (crc << 4) ^ t[crc >> 28];
	return crc;
}

uint32_t nanohub_crc32(const uint8_t *p, unsigned n)
{
	uint32_t crc = 0xffffffffu;
	unsigned i = 0;
	for (; i + 4 <= n; i += 4)
		crc = crc_word(crc, get32(p + i));
	if (i < n) {
		uint32_t w = 0;
		for (unsigned k = 0; i + k < n; k++)
			w |= (uint32_t)p[i + k] << (8 * k);
		crc = crc_word(crc, w);
	}
	return crc;
}

unsigned nanohub_encode(uint8_t *out, uint32_t seq, uint32_t reason, const uint8_t *payload, unsigned len)
{
	if (len > NANOHUB_PAYLOAD_MAX)
		return 0;
	out[0] = NANOHUB_SYNC;
	put32(out + 1, seq);
	put32(out + 5, reason);
	out[9] = (uint8_t)len;
	if (len)
		memcpy(out + NANOHUB_HEADER, payload, len);
	put32(out + NANOHUB_HEADER + len, nanohub_crc32(out, NANOHUB_HEADER + len));
	return NANOHUB_HEADER + len + 4;
}

int nanohub_decode(const uint8_t *buf, unsigned n, struct nanohub_packet *p, unsigned *used)
{
	unsigned i = 0;
	*used = 0;
	while (i < n && (buf[i] == 0xff || buf[i] == 0x00))
		i++;
	if (i == n)
		return NANOHUB_NEED_MORE;
	if (buf[i] != NANOHUB_SYNC) {
		*used = i + 1;
		return NANOHUB_BAD;
	}
	if (n - i < NANOHUB_HEADER)
		return NANOHUB_NEED_MORE;
	const uint8_t *b = buf + i;
	unsigned len = b[9], total = NANOHUB_HEADER + len + 4;
	if (n - i < total)
		return NANOHUB_NEED_MORE;
	if (get32(b + total - 4) != nanohub_crc32(b, total - 4)) {
		*used = i + 1;
		return NANOHUB_BAD;
	}
	p->seq = get32(b + 1);
	p->reason = get32(b + 5);
	p->len = (uint8_t)len;
	p->payload = b + NANOHUB_HEADER;
	*used = i + total;
	return NANOHUB_OK;
}

uint32_t nanohub_ack_intr(const struct nanohub_packet *p)
{
	return p->reason == NANOHUB_ACK && p->len == NANOHUB_ACK_INTR_SIZE ? get32(p->payload) : 0;
}

unsigned nanohub_read_payload(uint8_t *out, uint64_t boot_ns)
{
	put64(out, boot_ns);
	return 8;
}

unsigned nanohub_sensor_config(uint8_t *out, uint8_t type, uint8_t cmd, uint32_t rate, uint64_t latency_ns,
			       const uint8_t *data, unsigned dlen)
{
	if (dlen > NANOHUB_PAYLOAD_MAX - CONFIG_SIZE)
		return 0;
	put32(out, NANOHUB_EVT_CONFIG);
	put64(out + 4, latency_ns);
	put32(out + 12, rate);
	out[16] = type;
	out[17] = cmd;
	put16(out + 18, 0);
	if (dlen)
		memcpy(out + CONFIG_SIZE, data, dlen);
	return CONFIG_SIZE + dlen;
}

unsigned nanohub_time_config(uint8_t *out, uint64_t utc, int32_t gmtoff, bool h24)
{
	uint8_t t[TIME_SIZE];
	put64(t, utc);
	put32(t + 8, (uint32_t)gmtoff);
	put32(t + 12, h24 ? 1 : 0);
	return nanohub_sensor_config(out, NANOHUB_SENSOR_TIME, NANOHUB_CMD_CFG_DATA, 0, 0, t, sizeof(t));
}

static void gauge(const uint8_t *c, struct nanohub_fuel_gauge *g)
{
	g->status = c[0];
	g->present = c[1];
	g->control = get16(c + 2);
	g->temp_dc = (int16_t)get16(c + 4);
	g->mv = get16(c + 6);
	g->flags = get16(c + 8);
	g->remaining = get16(c + 12);
	g->full = get16(c + 14);
	g->current_ma = (int16_t)get16(c + 16);
	g->percent = get16(c + 30);
	g->power_mw = (int16_t)get16(c + 32);
	g->health = get16(c + 34);
	g->design_mah = get16(c + 36);
	g->discharging = g->flags & GAUGE_FLAG_DSG;
	g->full_charge = g->flags & GAUGE_FLAG_FC;
}

bool nanohub_event(const uint8_t *p, unsigned n, struct nanohub_event *ev)
{
	memset(ev, 0, sizeof(*ev));
	if (!n) {
		ev->kind = NANOHUB_EV_EMPTY;
		return true;
	}
	if (n < 4) {
		ev->kind = NANOHUB_EV_BAD;
		return false;
	}
	ev->type = get32(p) & 0x7fffffffu;
	ev->kind = NANOHUB_EV_OTHER;
	if (ev->type > NANOHUB_EVT_SENSOR && ev->type < NANOHUB_EVT_SENSOR + 0x100) {
		ev->kind = NANOHUB_EV_SENSOR;
		ev->sensor.type = (uint8_t)(ev->type - NANOHUB_EVT_SENSOR);
		if (n >= SENSOR_HEADER + 4) {
			ev->sensor.header = true;
			ev->sensor.ref_ns = get64(p + 4);
			ev->sensor.samples = p[SENSOR_HEADER];
			ev->sensor.flushes = p[SENSOR_HEADER + 1];
			ev->sensor.wake = p[SENSOR_HEADER + 3] == 1;
			ev->sensor.points = p + SENSOR_HEADER;
			ev->sensor.size = n - SENSOR_HEADER;
		}
	} else if (ev->type == NANOHUB_EVT_APP) {
		if (n < APP_HEADER || n < APP_HEADER + (unsigned)p[12]) {
			ev->kind = NANOHUB_EV_BAD;
			return false;
		}
		ev->kind = NANOHUB_EV_APP;
		ev->app.id = get64(p + 4);
		ev->app.data = p + APP_HEADER;
		ev->app.size = p[12];
		if (n == NANOHUB_FUEL_GAUGE_SIZE && ev->app.id == APP_ID_FUEL_GAUGE && ev->app.size == GAUGE_DATA_SIZE &&
		    ev->app.data[0] == APP_MSG_CUSTOM && ev->app.data[1] == NANOHUB_SENSOR_FUEL_GAUGE &&
		    ev->app.data[2] == 0) {
			ev->kind = NANOHUB_EV_FUEL_GAUGE;
			gauge(ev->app.data + GAUGE_CACHE, &ev->gauge);
		}
	} else if (ev->type == EVT_LOG || ev->type == EVT_LOG_NO_HAL) {
		ev->kind = NANOHUB_EV_LOG;
		if (n > 4)
			ev->log.level = (char)p[4];
		if (n > 5) {
			ev->log.text = p + 5;
			ev->log.size = n - 5;
		}
	}
	return true;
}

bool nanohub_sample(const struct nanohub_event *ev, unsigned i, unsigned data_size, uint64_t *ns, const uint8_t **data)
{
	if (ev->kind != NANOHUB_EV_SENSOR || !ev->sensor.header || i >= ev->sensor.samples)
		return false;
	unsigned step = 4 + data_size;
	if (ev->sensor.size < (i + 1) * step)
		return false;
	uint64_t t = ev->sensor.ref_ns;
	for (unsigned k = 1; k <= i; k++)
		t += (uint64_t)(get32(ev->sensor.points + k * step) & ~1u) << 9;
	*ns = t;
	*data = ev->sensor.points + i * step + 4;
	return true;
}
