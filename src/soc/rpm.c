/* Resource requests to the RPM processor over its shared-memory channel.
 * SMEM at 0x03000000 holds an allocation table; item 13 lists the SMD
 * channels, item 14+cid the channel's two stream descriptors (ours, then
 * the RPM's; eleven words each), item 338+cid the FIFO pair. A packet is
 * a five-word header (length first) plus payload; the RPM is told about
 * new data through the APCS IPC register and answers in the second FIFO.
 * Regulators on this SoC are RPM resources, so enabling an LDO is such a
 * request: type "ldoa"/"vsa", the regulator number, key "swen" = 1. */
#include <string.h>
#include "asteroid.h"

#define SMEM_BASE		0x03000000u
#define SMEM_ALLOC_TABLE	(SMEM_BASE + 0xd0)
#define SMEM_CHANNEL_ALLOC_TBL	13
#define SMEM_SMD_BASE_ID	14
#define SMEM_SMD_FIFO_BASE_ID	338
#define SMD_EDGE_RPM		15
#define APCS_IPC		0xf9011008u
#define SS_OPENING		1
#define SS_OPENED		2
#define RPM_REQ			0x00716572u

struct alloc_entry { uint32_t allocated, offset, size, base_ext; };
struct chan_alloc { char name[20]; uint32_t cid, ctype, ref_count; };
struct stream {
	uint32_t state, dsr, cts, cd, ri, data_written, data_read, state_updated, mask_recv_intr, read_index, write_index;
};
struct port { struct stream local, remote; };

static volatile struct port *port;
static volatile uint32_t *tx, *rx;
static uint32_t fifo_size, msg_id;
static bool ready;

/* An entry's base_ext names an auxiliary region (the RPM's message RAM
 * at 0xfc428000 for the RPM edge); otherwise the offset is from SMEM. */
static void *smem_item(unsigned id, uint32_t *size)
{
	volatile struct alloc_entry *e = (volatile struct alloc_entry *)(uintptr_t)(SMEM_ALLOC_TABLE + id * 16);
	if (!e->allocated || !e->size)
		return NULL;
	if (size)
		*size = e->size;
	uint32_t base = e->base_ext ? e->base_ext : SMEM_BASE;
	log_printf("smem: item %u at %08lx + %08lx size %08lx (ext %08lx)\n", id, base, e->offset, e->size, e->base_ext);
	return (void *)(uintptr_t)(base + e->offset);
}

static void notify(void)
{
	dsb();
	writel(1, APCS_IPC);
}

bool rpm_init(void)
{
	uint32_t size;
	volatile struct chan_alloc *tbl = smem_item(SMEM_CHANNEL_ALLOC_TBL, &size);
	if (!tbl) {
		for (uint32_t base = 0x03000000; base <= 0x0fa00000; base += 0x0ca00000) {
			volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)base;
			log_printf("smem %08lx: ver %08lx %08lx heap %lu/%08lx/%08lx item13 %08lx %08lx %08lx item14 %08lx %08lx %08lx\n",
				   base, w[16], w[17], w[48], w[49], w[50], w[52 + 13 * 4], w[53 + 13 * 4], w[54 + 13 * 4],
				   w[52 + 14 * 4], w[53 + 14 * 4], w[54 + 14 * 4]);
		}
		log_printf("rpm: no channel table in smem\n");
		return false;
	}
	int cid = -1;
	for (unsigned i = 0; i < size / sizeof(struct chan_alloc); i++) {
		if ((tbl[i].ctype & 0xff) == SMD_EDGE_RPM && !strncmp((const char *)tbl[i].name, "rpm_requests", 12)) {
			cid = (int)tbl[i].cid;
			break;
		}
	}
	if (cid < 0) {
		log_printf("rpm: no rpm_requests channel\n");
		return false;
	}
	uint32_t info_size = 0;
	port = smem_item(SMEM_SMD_BASE_ID + cid, &info_size);
	volatile uint32_t *fifo = smem_item(SMEM_SMD_FIFO_BASE_ID + cid, &size);
	if (!port || !fifo) {
		log_printf("rpm: channel %d has no descriptors\n", cid);
		return false;
	}
	fifo_size = size / 2;
	tx = fifo;
	rx = fifo + fifo_size / 4;
	log_printf("rpm: channel %d info %lu fifo %lu, local state %lu remote %lu, idx %lu/%lu %lu/%lu\n", cid, info_size, fifo_size,
		   port->local.state, port->remote.state, port->local.write_index, port->remote.read_index,
		   port->remote.write_index, port->local.read_index);
	if (port->local.state != SS_OPENED) {
		port->local.mask_recv_intr = 1;
		port->local.state = SS_OPENING;
		port->local.dsr = port->local.cts = port->local.cd = 1;
		port->local.write_index = 0;
		port->remote.read_index = 0;
		port->local.state_updated = 1;
		notify();
		uint32_t t0 = time_ms();
		while (port->remote.state != SS_OPENING && port->remote.state != SS_OPENED && time_ms() - t0 < 500)
			;
		log_printf("rpm: remote state %lu after %lu ms\n", port->remote.state, time_ms() - t0);
		port->local.state = SS_OPENED;
		port->local.state_updated = 1;
		notify();
		t0 = time_ms();
		while (port->remote.state != SS_OPENED && time_ms() - t0 < 500)
			;
		log_printf("rpm: opened, remote state %lu after %lu ms\n", port->remote.state, time_ms() - t0);
	}
	ready = port->remote.state == SS_OPENED;
	return ready;
}

static void fifo_write(const uint32_t *src, unsigned words)
{
	uint32_t w = port->local.write_index;
	for (unsigned i = 0; i < words; i++) {
		tx[w / 4] = src[i];
		w += 4;
		if (w >= fifo_size)
			w = 0;
	}
	port->local.write_index = w;
}

static void fifo_read(uint32_t *dst, unsigned words)
{
	uint32_t r = port->remote.read_index;
	for (unsigned i = 0; i < words; i++) {
		dst[i] = rx[r / 4];
		r += 4;
		if (r >= fifo_size)
			r = 0;
	}
	port->remote.read_index = r;
}

static uint32_t rx_avail(void)
{
	uint32_t w = port->remote.write_index, r = port->remote.read_index;
	return w >= r ? w - r : fifo_size - r + w;
}

int rpm_request(uint32_t type, uint32_t id, const uint32_t *kvp, unsigned nkvp)
{
	if (!ready)
		return -1;
	uint32_t pkt[5 + 7 + 3 * 4];
	unsigned n = 0;
	uint32_t data_len = nkvp * 12;
	pkt[n++] = 28 + data_len; pkt[n++] = 0; pkt[n++] = 0; pkt[n++] = 0; pkt[n++] = 0;
	pkt[n++] = RPM_REQ; pkt[n++] = 20 + data_len;
	pkt[n++] = ++msg_id; pkt[n++] = 0; pkt[n++] = type; pkt[n++] = id; pkt[n++] = data_len;
	for (unsigned i = 0; i < nkvp * 3; i++)
		pkt[n++] = kvp[i];
	port->remote.data_read = 0;
	fifo_write(pkt, n);
	port->local.data_written = 1;
	port->local.mask_recv_intr = 0;
	notify();
	uint32_t t0 = time_ms();
	while (rx_avail() < 20) {
		if (time_ms() - t0 > 500) {
			log_printf("rpm: no answer to msg %lu\n", msg_id);
			return -2;
		}
	}
	uint32_t hdr[5], resp[8] = { 0 };
	fifo_read(hdr, 5);
	while (rx_avail() < hdr[0] && time_ms() - t0 < 500)
		;
	unsigned words = hdr[0] / 4;
	if (words > 8)
		words = 8;
	fifo_read(resp, words);
	port->remote.data_written = 0;
	port->local.data_read = 1;
	notify();
	log_printf("rpm: msg %lu -> %lu bytes: %08lx %08lx %08lx %08lx %08lx\n", msg_id, hdr[0],
		   resp[0], resp[1], resp[2], resp[3], resp[4]);
	return resp[0] == RPM_REQ ? 0 : -3;
}

int rpm_regulator_enable(uint32_t type, uint32_t id, bool on)
{
	uint32_t kvp[3] = { 0x6e657773, 4, on };
	return rpm_request(type, id, kvp, 1);
}

/* Bus and memory clocks: an RPM vote outlives a warm reset, so set the
 * apps processor's votes explicitly at boot. */
#define RPM_CLK1		0x316b6c63u
#define RPM_CLK2		0x326b6c63u
#define RPM_KEY_SWEN		0x6e657773u
#define RPM_KEY_KHZ		0x007a484bu

int rpm_clock_rate(uint32_t type, uint32_t id, uint32_t khz)
{
	uint32_t kvp[6] = { RPM_KEY_SWEN, 4, 1, RPM_KEY_KHZ, 4, khz };
	return rpm_request(type, id, kvp, 2);
}

void rpm_bus_votes(void)
{
	rpm_clock_rate(RPM_CLK2, 0, 200000);
	rpm_clock_rate(RPM_CLK1, 1, 100000);
	rpm_clock_rate(RPM_CLK1, 0, 50000);
	rpm_clock_rate(RPM_CLK1, 2, 50000);
}

