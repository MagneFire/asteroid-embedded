#include <string.h>
#include "bleif_proto.h"

#define CHIPREV_MASK	0xffu
#define CHIPREV_A1	0x12u
#define CHIPREV_B0	0x21u

#define EV_COMMAND_COMPLETE	0x0e
#define READ_LOCAL_FEATURES	0x1003
/* LE supported, BR/EDR not supported: the controller reports neither,
 * and the host gives up on a controller without them. */
#define FEATURES_LE_ONLY	0x60

static const struct bleif_patch *const a1[] = { &apollo3_ble_a1_perf, &apollo3_ble_a1_function, &apollo3_ble_a1_nvds };
static const struct bleif_patch *const b0[] = { &apollo3_ble_b0_perf, &apollo3_ble_b0_function, &apollo3_ble_b0_nvds };

static const struct bleif_rev revs[] = {
	{ "A1", &apollo3_ble_a1_copy, a1, 3, &apollo3_ble_a1_perf, &apollo3_ble_a1_nvds, 0x20006054, 0x20006070, 0x20006038 },
	{ "B0", NULL, b0, 3, &apollo3_ble_b0_perf, &apollo3_ble_b0_nvds, 0x20006858, 0x20006874, 0x20006838 },
};

const struct bleif_rev *bleif_rev(uint32_t chiprev)
{
	switch (chiprev & CHIPREV_MASK) {
	case CHIPREV_A1:
		return &revs[0];
	case CHIPREV_B0:
		return &revs[1];
	default:
		return NULL;
	}
}

void bleif_edit(struct bleif_edits *ed, unsigned off, const uint8_t *v, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		unsigned k = 0;
		while (k < ed->n && ed->e[k].off != off + i)
			k++;
		if (k == BLEIF_EDITS)
			return;
		if (k == ed->n)
			ed->n++;
		ed->e[k].off = (uint16_t)(off + i);
		ed->e[k].val = v[i];
	}
}

void bleif_patch_read(const struct bleif_patch *p, const struct bleif_edits *ed, unsigned off, unsigned n,
		      uint8_t *out)
{
	memcpy(out, p->data + off, n);
	for (unsigned k = 0; ed && k < ed->n; k++)
		if (ed->e[k].off >= off && ed->e[k].off < off + n)
			out[ed->e[k].off - off] = ed->e[k].val;
}

/* CRC-16, polynomial 0x8005, MSB first, from 0. */
uint16_t bleif_crc(uint16_t crc, const uint8_t *p, unsigned n)
{
	while (n--) {
		crc ^= (uint16_t)(*p++ << 8);
		for (int b = 0; b < 8; b++)
			crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x8005) : (uint16_t)(crc << 1);
	}
	return crc;
}

uint16_t bleif_patch_crc(const struct bleif_patch *p, const struct bleif_edits *ed)
{
	uint8_t buf[BLEIF_PATCH_CHUNK];
	uint16_t crc = 0;
	for (unsigned off = 0; off < p->len; off += sizeof buf) {
		unsigned n = p->len - off < sizeof buf ? p->len - off : sizeof buf;
		bleif_patch_read(p, ed, off, n, buf);
		crc = bleif_crc(crc, buf, n);
	}
	return crc;
}

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> 8 * i);
}

static uint32_t swap32(uint32_t v)
{
	return v << 24 | (v & 0xff00) << 8 | (v >> 8 & 0xff00) | v >> 24;
}

/* The RAM code's words 2 and 3 take them, byte-swapped; the retention
 * voltage one step up, past 0.75 V. */
void bleif_trims(const struct bleif_patch *p, uint32_t bandgap, uint32_t retention, struct bleif_edits *ed)
{
	uint8_t w[4];
	uint32_t v = swap32(bandgap);
	if (v != 0xffffffffu) {
		put32(w, v);
		bleif_edit(ed, 8, w, 4);
	}
	if (retention != 0xffffffffu)
		retention += 0x40000000u;
	v = swap32(retention);
	if (v != 0xffffffffu) {
		put32(w, (get32(p->data + 12) & ~0x1f0u) | v);
		bleif_edit(ed, 12, w, 4);
	}
}

/* After a 4-byte magic, parameters as tag, 0x06, length, value. */
int bleif_nvds_set(const struct bleif_patch *p, uint8_t tag, const uint8_t *v, unsigned n, struct bleif_edits *ed)
{
	for (unsigned off = 4; off + 3 <= p->len;) {
		uint8_t t = p->data[off], len = p->data[off + 2];
		if (!t && !len)
			break;
		if (t == tag && len == n && off + 3 + n <= p->len) {
			bleif_edit(ed, off + 3, v, n);
			return 0;
		}
		off += 3u + len;
	}
	return -1;
}

/* Valid with 0x182406 in the top bytes and the trim within 0x20-0x40. */
uint8_t bleif_modex_trim(uint32_t word)
{
	uint8_t t = (uint8_t)word;
	if ((word & 0xffffff00u) != 0x18240600u || t < 0x20 || t > 0x40)
		return 0;
	return t;
}

/* CHIPID1 and bits 8-23 of CHIPID0, least significant byte first. */
void bleif_chip_addr(uint32_t id0, uint32_t id1, uint8_t addr[6])
{
	uint8_t lsb[6] = { (uint8_t)id1, (uint8_t)(id1 >> 8), (uint8_t)(id1 >> 16), (uint8_t)(id1 >> 24),
			   (uint8_t)(id0 >> 8), (uint8_t)(id0 >> 16) };
	for (int i = 0; i < 6; i++)
		addr[i] = lsb[5 - i];
}

unsigned bleif_patch_packet(uint8_t *out, uint8_t type, uint8_t step, const uint8_t *p, unsigned n)
{
	out[0] = 0x01;
	out[1] = type;
	out[2] = step;
	out[3] = (uint8_t)n;
	memcpy(out + 4, p, n);
	return 4 + n;
}

bool bleif_patch_ack(const uint8_t *r, uint8_t type, uint8_t step)
{
	return r[0] == 0x04 && r[1] == type && r[2] == step && r[3] == 0x01 && r[4] == 0x00;
}

bool bleif_packet_ok(const uint8_t *p, unsigned n)
{
	switch (n ? p[0] : 0) {
	case 0x04:
		return n >= 3 && 3u + p[2] == n;
	case 0x02:
		return n >= 5 && 5u + (p[3] | p[4] << 8) == n;
	default:
		return false;
	}
}

void bleif_fix_event(uint8_t *p, unsigned n)
{
	if (n >= 14 && p[0] == EV_COMMAND_COMPLETE && (p[3] | p[4] << 8) == READ_LOCAL_FEATURES && !p[5])
		p[6 + 4] |= FEATURES_LE_ONLY;
}
