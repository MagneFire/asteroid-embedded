/* Device side of the Android Debug Bridge over a packet transport. Every
 * message is a 24-byte header, sent as its own transfer, followed by its
 * payload as a second one. Incoming bytes are reassembled into messages;
 * replies are queued and drained one transfer at a time by adb_poll, so
 * adb_rx itself sends at most once. Services: an interactive console
 * ("shell:"), one-shot console commands ("shell:<cmd>"), "reboot:" and
 * the file sync protocol against the RAM file table. No authentication. */
#include <string.h>
#include "asteroid.h"
#include "console/console.h"
#include "adb.h"

#define A_CNXN	0x4e584e43u
#define A_AUTH	0x48545541u
#define A_OPEN	0x4e45504fu
#define A_OKAY	0x59414b4fu
#define A_CLSE	0x45534c43u
#define A_WRTE	0x45545257u
#define VERSION	0x01000000u
#define HDR	24

#define ID(a, b, c, d)	((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define ID_SEND	ID('S', 'E', 'N', 'D')
#define ID_RECV	ID('R', 'E', 'C', 'V')
#define ID_STAT	ID('S', 'T', 'A', 'T')
#define ID_LIST	ID('L', 'I', 'S', 'T')
#define ID_DENT	ID('D', 'E', 'N', 'T')
#define ID_DATA	ID('D', 'A', 'T', 'A')
#define ID_DONE	ID('D', 'O', 'N', 'E')
#define ID_OKAY	ID('O', 'K', 'A', 'Y')
#define ID_FAIL	ID('F', 'A', 'I', 'L')
#define ID_QUIT	ID('Q', 'U', 'I', 'T')

#define STREAMS		4
#define RING		4096
#define CTRL_QUEUE	16
#define BANNER_MAX	256
#define S_IFREG		0100000u

enum { FREE, SHELL, EXEC, REBOOT, SYNC };
enum { SY_HDR, SY_PATH, SY_DATA, SY_DEAD };
enum { RP_NONE, RP_RECV, RP_LIST };

struct stream {
	uint8_t kind;
	bool waiting, closing;
	uint32_t local, remote;
	uint8_t ring[RING];
	uint32_t head, count;
	uint8_t sstate, reply;
	uint8_t hdr[8];
	uint32_t hdr_len, id, len, got;
	char path[ADB_NAME_MAX];
	bool sending, send_err;
	struct adb_file *file;
	uint32_t off;
	unsigned idx;
};

struct ctrl {
	uint32_t cmd, arg0, arg1;
};

static const struct adb_transport *transport;
static char banner[BANNER_MAX];
static uint32_t banner_len;
static bool online, cnxn_pending, reboot_pending;
static uint32_t host_max = ADB_MAX_PAYLOAD, next_id = 1;
static struct stream streams[STREAMS];
static unsigned rr;

static struct ctrl ctrl[CTRL_QUEUE];
static unsigned ctrl_head, ctrl_count;

static uint8_t tx[HDR + ADB_MAX_PAYLOAD];
static uint32_t tx_len, tx_off;
static bool tx_reboot;

static uint8_t rx_hdr[HDR];
static uint32_t rx_hdr_len, rx_got;
static bool rx_payload, rx_skip;
static uint32_t rx_cmd, rx_arg0, rx_arg1, rx_len;
static uint8_t rx_data[ADB_RX_MAX];

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t ring_free(const struct stream *s) { return RING - s->count; }

static uint32_t ring_put(struct stream *s, const void *buf, uint32_t n)
{
	const uint8_t *p = buf;
	if (n > ring_free(s))
		n = ring_free(s);
	for (uint32_t i = 0; i < n; i++)
		s->ring[(s->head + s->count + i) % RING] = p[i];
	s->count += n;
	return n;
}

static void ring_put32(struct stream *s, uint32_t v)
{
	uint8_t b[4];
	put32(b, v);
	ring_put(s, b, 4);
}

static void queue_ctrl(uint32_t cmd, uint32_t arg0, uint32_t arg1)
{
	if (ctrl_count == CTRL_QUEUE)
		return;
	struct ctrl *c = &ctrl[(ctrl_head + ctrl_count++) % CTRL_QUEUE];
	c->cmd = cmd;
	c->arg0 = arg0;
	c->arg1 = arg1;
}

static void exec_done(void *ctx)
{
	((struct stream *)ctx)->closing = true;
}

static void stream_free(struct stream *s)
{
	if (s->kind == SHELL)
		console_detach();
	if (s->kind == EXEC)
		console_cancel(s);
	if (s->kind == SYNC && s->sending && s->file)
		adb_fs_remove(s->file);
	s->kind = FREE;
}

static struct stream *stream_find(uint32_t local)
{
	for (unsigned i = 0; i < STREAMS; i++)
		if (streams[i].kind != FREE && streams[i].local == local)
			return &streams[i];
	return NULL;
}

static void drop_all(void)
{
	for (unsigned i = 0; i < STREAMS; i++)
		if (streams[i].kind != FREE)
			stream_free(&streams[i]);
	ctrl_count = 0;
	cnxn_pending = false;
}

static void stream_out(const char *s, size_t n, void *ctx)
{
	ring_put(ctx, s, (uint32_t)n);
}

static void sync_status(struct stream *s, uint32_t id, const char *msg)
{
	uint32_t n = msg ? (uint32_t)strlen(msg) : 0;
	ring_put32(s, id);
	ring_put32(s, n);
	ring_put(s, msg, n);
}

static void sync_fail(struct stream *s, const char *msg, bool fatal)
{
	sync_status(s, ID_FAIL, msg);
	if (fatal) {
		s->sstate = SY_DEAD;
		s->closing = true;
	}
}

static void sync_produce(struct stream *s)
{
	if (s->reply == RP_RECV) {
		struct adb_file *f = s->file;
		while (s->off < f->size && ring_free(s) > 8) {
			uint32_t n = f->size - s->off;
			if (n > ring_free(s) - 8)
				n = ring_free(s) - 8;
			ring_put32(s, ID_DATA);
			ring_put32(s, n);
			ring_put(s, f->data + s->off, n);
			s->off += n;
		}
		if (s->off == f->size && ring_free(s) >= 8) {
			sync_status(s, ID_DONE, NULL);
			s->file = NULL;
			s->reply = RP_NONE;
		}
	} else if (s->reply == RP_LIST) {
		size_t plen = strlen(s->path);
		bool slash = plen && s->path[plen - 1] == '/';
		for (; s->idx < ADB_FILES; s->idx++) {
			struct adb_file *f = adb_fs_entry(s->idx);
			if (!f || strncmp(f->name, s->path, plen))
				continue;
			const char *name = f->name + plen;
			if (!slash) {
				if (*name != '/')
					continue;
				name++;
			}
			uint32_t n = (uint32_t)strlen(name);
			if (!n)
				continue;
			if (ring_free(s) < 20 + n)
				return;
			ring_put32(s, ID_DENT);
			ring_put32(s, f->mode);
			ring_put32(s, f->size);
			ring_put32(s, f->mtime);
			ring_put32(s, n);
			ring_put(s, name, n);
		}
		if (ring_free(s) < 20)
			return;
		ring_put32(s, ID_DONE);
		for (int i = 0; i < 4; i++)
			ring_put32(s, 0);
		s->reply = RP_NONE;
	}
}

static void sync_request(struct stream *s)
{
	s->sstate = SY_HDR;
	if (s->id == ID_SEND) {
		char *comma = NULL;
		for (char *p = s->path; *p; p++)
			if (*p == ',')
				comma = p;
		uint32_t mode = S_IFREG | 0644;
		if (comma) {
			*comma = 0;
			mode = (uint32_t)strtoul(comma + 1, NULL, 10);
		}
		if (!(mode & 0170000))
			mode |= S_IFREG;
		s->sending = true;
		s->file = adb_fs_create(s->path, mode);
		s->send_err = !s->file;
	} else if (s->id == ID_RECV) {
		s->file = adb_fs_find(s->path);
		if (!s->file)
			sync_fail(s, "No such file or directory", false);
		else {
			s->off = 0;
			s->reply = RP_RECV;
		}
	} else if (s->id == ID_STAT) {
		struct adb_file *f = adb_fs_find(s->path);
		ring_put32(s, ID_STAT);
		ring_put32(s, f ? f->mode : 0);
		ring_put32(s, f ? f->size : 0);
		ring_put32(s, f ? f->mtime : 0);
	} else {
		s->idx = 0;
		s->reply = RP_LIST;
	}
	sync_produce(s);
}

static void sync_header(struct stream *s)
{
	s->id = get32(s->hdr);
	s->len = get32(s->hdr + 4);
	s->got = 0;
	if (s->id == ID_SEND || s->id == ID_RECV || s->id == ID_STAT || s->id == ID_LIST) {
		if (s->sending || s->reply != RP_NONE)
			sync_fail(s, "sync request while busy", true);
		else if (s->len >= ADB_NAME_MAX)
			sync_fail(s, "path too long", true);
		else if (s->len == 0) {
			s->path[0] = 0;
			sync_request(s);
		} else
			s->sstate = SY_PATH;
	} else if (s->id == ID_DATA) {
		if (!s->sending)
			sync_fail(s, "DATA without SEND", true);
		else if (s->len)
			s->sstate = SY_DATA;
	} else if (s->id == ID_DONE) {
		if (!s->sending) {
			sync_fail(s, "DONE without SEND", true);
			return;
		}
		s->sending = false;
		if (s->send_err) {
			if (s->file)
				adb_fs_remove(s->file);
			sync_fail(s, s->file ? "file too large" : "no space for file", false);
		} else {
			s->file->mtime = s->len;
			sync_status(s, ID_OKAY, NULL);
		}
		s->file = NULL;
	} else if (s->id == ID_QUIT) {
		s->sstate = SY_DEAD;
		s->closing = true;
	} else
		sync_fail(s, "unknown sync command", true);
}

static void sync_input(struct stream *s, const uint8_t *p, uint32_t n)
{
	while (n && s->sstate != SY_DEAD) {
		uint32_t k;
		if (s->sstate == SY_HDR) {
			k = 8 - s->hdr_len;
			if (k > n)
				k = n;
			memcpy(s->hdr + s->hdr_len, p, k);
			s->hdr_len += k;
			if (s->hdr_len == 8) {
				s->hdr_len = 0;
				sync_header(s);
			}
		} else if (s->sstate == SY_PATH) {
			k = s->len - s->got;
			if (k > n)
				k = n;
			memcpy(s->path + s->got, p, k);
			s->got += k;
			if (s->got == s->len) {
				s->path[s->len] = 0;
				sync_request(s);
			}
		} else {
			k = s->len - s->got;
			if (k > n)
				k = n;
			struct adb_file *f = s->file;
			if (!s->send_err) {
				if (f->size + k > ADB_FILE_MAX)
					s->send_err = true;
				else {
					memcpy(f->data + f->size, p, k);
					f->size += k;
				}
			}
			s->got += k;
			if (s->got == s->len)
				s->sstate = SY_HDR;
		}
		p += k;
		n -= k;
	}
}

static bool shell_attached(void)
{
	for (unsigned i = 0; i < STREAMS; i++)
		if (streams[i].kind == SHELL)
			return true;
	return false;
}

static void handle_open(void)
{
	char *svc = (char *)rx_data;
	if (!rx_len) {
		queue_ctrl(A_CLSE, 0, rx_arg0);
		return;
	}
	if (rx_len == ADB_RX_MAX)
		rx_len--;
	rx_data[rx_len] = 0;
	struct stream *s = NULL;
	for (unsigned i = 0; !s && i < STREAMS; i++)
		if (streams[i].kind == FREE)
			s = &streams[i];
	uint8_t kind = FREE;
	if (!strcmp(svc, "shell:") || !strcmp(svc, "shell"))
		kind = shell_attached() ? FREE : SHELL;
	else if (!strncmp(svc, "shell:", 6))
		kind = EXEC;
	else if (!strncmp(svc, "reboot:", 7))
		kind = REBOOT;
	else if (!strcmp(svc, "sync:"))
		kind = SYNC;
	if (!s || kind == FREE) {
		queue_ctrl(A_CLSE, 0, rx_arg0);
		return;
	}
	s->kind = kind;
	s->local = next_id++;
	if (!next_id)
		next_id = 1;
	s->remote = rx_arg0;
	s->head = s->count = 0;
	s->waiting = s->closing = false;
	s->sstate = SY_HDR;
	s->reply = RP_NONE;
	s->hdr_len = 0;
	s->sending = s->send_err = false;
	s->file = NULL;
	queue_ctrl(A_OKAY, s->local, s->remote);
	if (kind == SHELL)
		console_attach(stream_out, s, true);
	else if (kind == EXEC) {
		if (!console_exec(svc + 6, stream_out, s, exec_done)) {
			ring_put(s, "busy\n", 5);
			s->closing = true;
		}
	} else if (kind == REBOOT)
		s->closing = true;
}

static void handle_write(struct stream *s)
{
	if (s->kind == SHELL) {
		const char *p = (const char *)rx_data;
		uint32_t n = rx_len;
		for (uint32_t i = 0; i < rx_len; i++)
			if (p[i] == 0x04) {
				n = i;
				s->closing = true;
				break;
			}
		console_input(p, n);
		if (s->closing)
			console_detach();
	} else if (s->kind == SYNC)
		sync_input(s, rx_data, rx_len);
	queue_ctrl(A_OKAY, s->local, s->remote);
}

static void dispatch(void)
{
	if (rx_cmd == A_CNXN) {
		drop_all();
		host_max = rx_arg1 && rx_arg1 < ADB_MAX_PAYLOAD ? rx_arg1 : ADB_MAX_PAYLOAD;
		cnxn_pending = true;
		if (!online)
			log_printf("adb: host connected, max payload %lu\n", rx_arg1);
		online = true;
		return;
	}
	if (!online)
		return;
	if (rx_cmd == A_OPEN) {
		handle_open();
		return;
	}
	struct stream *s = stream_find(rx_arg1);
	if (!s || (s->remote != rx_arg0 && rx_arg0))
		return;
	if (rx_cmd == A_WRTE)
		handle_write(s);
	else if (rx_cmd == A_OKAY)
		s->waiting = false;
	else if (rx_cmd == A_CLSE) {
		queue_ctrl(A_CLSE, s->local, s->remote);
		stream_free(s);
	}
}

static void build(uint32_t cmd, uint32_t arg0, uint32_t arg1, uint32_t len)
{
	uint32_t sum = 0;
	for (uint32_t i = 0; i < len; i++)
		sum += tx[HDR + i];
	put32(tx, cmd);
	put32(tx + 4, arg0);
	put32(tx + 8, arg1);
	put32(tx + 12, len);
	put32(tx + 16, sum);
	put32(tx + 20, cmd ^ 0xffffffffu);
	tx_len = HDR + len;
	tx_off = 0;
}

static bool next_message(void)
{
	if (cnxn_pending) {
		cnxn_pending = false;
		memcpy(tx + HDR, banner, banner_len);
		build(A_CNXN, VERSION, ADB_MAX_PAYLOAD, banner_len);
		return true;
	}
	if (ctrl_count) {
		struct ctrl *c = &ctrl[ctrl_head];
		ctrl_head = (ctrl_head + 1) % CTRL_QUEUE;
		ctrl_count--;
		build(c->cmd, c->arg0, c->arg1, 0);
		return true;
	}
	for (unsigned k = 0; k < STREAMS; k++) {
		struct stream *s = &streams[(rr + k) % STREAMS];
		if (s->kind == FREE || s->waiting)
			continue;
		if (s->kind == SYNC)
			sync_produce(s);
		if (s->count) {
			uint32_t n = s->count < host_max ? s->count : host_max;
			for (uint32_t i = 0; i < n; i++)
				tx[HDR + i] = s->ring[(s->head + i) % RING];
			s->head = (s->head + n) % RING;
			s->count -= n;
			s->waiting = true;
			build(A_WRTE, s->local, s->remote, n);
		} else if (s->closing) {
			build(A_CLSE, s->local, s->remote, 0);
			tx_reboot = s->kind == REBOOT;
			stream_free(s);
		} else
			continue;
		rr = (rr + k + 1) % STREAMS;
		return true;
	}
	return false;
}

static bool step(void)
{
	if (!transport || (tx_off == tx_len && !next_message()))
		return false;
	uint32_t n = tx_off < HDR ? HDR : tx_len - HDR;
	if (!transport->send(tx + tx_off, n))
		return false;
	tx_off += n;
	if (tx_off == tx_len) {
		tx_off = tx_len = 0;
		if (tx_reboot)
			reboot_pending = true;
		tx_reboot = false;
	}
	return true;
}

void adb_init(const struct adb_transport *t, const char *banner_props)
{
	transport = t;
	int n = snprintf(banner, sizeof banner, "device::%s;features=", banner_props ? banner_props : "");
	banner_len = n < (int)sizeof banner ? (uint32_t)n : sizeof banner - 1;
	adb_reset();
}

void adb_reset(void)
{
	drop_all();
	tx_len = tx_off = 0;
	tx_reboot = reboot_pending = false;
	online = false;
	host_max = ADB_MAX_PAYLOAD;
	rx_hdr_len = 0;
	rx_payload = false;
}

bool adb_online(void) { return online; }

uint32_t adb_rx_expected(void)
{
	return rx_payload ? rx_len - rx_got : HDR - rx_hdr_len;
}

void adb_rx(const void *buf, uint32_t len)
{
	const uint8_t *p = buf;
	while (len) {
		uint32_t k;
		if (!rx_payload) {
			k = HDR - rx_hdr_len;
			if (k > len)
				k = len;
			memcpy(rx_hdr + rx_hdr_len, p, k);
			rx_hdr_len += k;
			p += k;
			len -= k;
			if (rx_hdr_len < HDR)
				continue;
			rx_hdr_len = 0;
			rx_cmd = get32(rx_hdr);
			if (get32(rx_hdr + 20) != (rx_cmd ^ 0xffffffffu))
				break;
			rx_arg0 = get32(rx_hdr + 4);
			rx_arg1 = get32(rx_hdr + 8);
			rx_len = get32(rx_hdr + 12);
			rx_skip = rx_len > ADB_RX_MAX;
			rx_got = 0;
			if (rx_len)
				rx_payload = true;
			else
				dispatch();
		} else {
			k = rx_len - rx_got;
			if (k > len)
				k = len;
			if (!rx_skip)
				memcpy(rx_data + rx_got, p, k);
			rx_got += k;
			p += k;
			len -= k;
			if (rx_got == rx_len) {
				rx_payload = false;
				if (!rx_skip)
					dispatch();
			}
		}
	}
	step();
}

void adb_poll(void)
{
	for (int i = 0; i < 16 && step(); i++)
		;
	if (reboot_pending) {
		log_printf("adb: rebooting\n");
		reboot();
	}
}
