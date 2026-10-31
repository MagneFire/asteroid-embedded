/* adb device side against a fake host on the other end of the transport. */
#include <stdio.h>
#include <string.h>
#include "asteroid.h"
#include "usb/adb.h"
#include "console/console.h"
#include "stubs.h"

#define A_CNXN	0x4e584e43u
#define A_AUTH	0x48545541u
#define A_OPEN	0x4e45504fu
#define A_OKAY	0x59414b4fu
#define A_CLSE	0x45534c43u
#define A_WRTE	0x45545257u
#define ID(s)	((uint32_t)(s)[0] | (uint32_t)(s)[1] << 8 | (uint32_t)(s)[2] << 16 | (uint32_t)(s)[3] << 24)
#define PROPS	"ro.product.name=sturgeon;ro.product.model=asteroid-embedded;ro.product.device=sturgeon"

struct msg {
	uint32_t cmd, arg0, arg1, len;
	uint8_t data[ADB_MAX_PAYLOAD];
};

static struct msg msgs[64];
static unsigned msg_head, msg_count;
static struct msg cur;
static bool want_payload;
static uint32_t cur_check;
static int busy_mode, calls;

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static void push_msg(void)
{
	CHECK(msg_count < 64);
	msgs[(msg_head + msg_count++) % 64] = cur;
}

static bool fake_send(const void *buf, uint32_t len)
{
	calls++;
	if (busy_mode == 1 || (busy_mode == 2 && calls % 2))
		return false;
	const uint8_t *p = buf;
	if (!want_payload) {
		CHECK(len == 24);
		cur.cmd = get32(p);
		cur.arg0 = get32(p + 4);
		cur.arg1 = get32(p + 8);
		cur.len = get32(p + 12);
		CHECK(get32(p + 20) == (cur.cmd ^ 0xffffffffu));
		CHECK(cur.len <= ADB_MAX_PAYLOAD);
		cur_check = get32(p + 16);
		if (cur.len)
			want_payload = true;
		else
			push_msg();
	} else {
		uint32_t sum = 0;
		CHECK(len == cur.len);
		for (uint32_t i = 0; i < len; i++)
			sum += p[i];
		CHECK(sum == cur_check);
		memcpy(cur.data, p, len);
		want_payload = false;
		push_msg();
	}
	return true;
}

static const struct adb_transport transport = { fake_send };

static void host_send_split(uint32_t cmd, uint32_t a0, uint32_t a1, const void *data, uint32_t len, uint32_t split)
{
	uint8_t h[24];
	put32(h, cmd);
	put32(h + 4, a0);
	put32(h + 8, a1);
	put32(h + 12, len);
	put32(h + 16, 0);
	put32(h + 20, cmd ^ 0xffffffffu);
	CHECK(adb_rx_expected() == 24);
	adb_rx(h, 24);
	const uint8_t *p = data;
	while (len) {
		uint32_t k = len < split ? len : split;
		if (len <= ADB_RX_MAX)
			CHECK(adb_rx_expected() == len);
		adb_rx(p, k);
		p += k;
		len -= k;
	}
	CHECK(adb_rx_expected() == 24);
}

static void host_send(uint32_t cmd, uint32_t a0, uint32_t a1, const void *data, uint32_t len)
{
	host_send_split(cmd, a0, a1, data, len, 512);
}

static void pump(void)
{
	console_poll();
	for (int i = 0; i < 8; i++)
		adb_poll();
}

static bool pop(struct msg *m)
{
	if (!msg_count)
		return false;
	*m = msgs[msg_head];
	msg_head = (msg_head + 1) % 64;
	msg_count--;
	return true;
}

static bool get(struct msg *m)
{
	pump();
	return pop(m);
}

static void expect(uint32_t cmd, uint32_t a0, uint32_t a1, const char *data, uint32_t len)
{
	struct msg m;
	bool ok = get(&m);
	CHECK(ok);
	if (!ok)
		return;
	CHECK(m.cmd == cmd);
	CHECK(m.arg0 == a0);
	CHECK(m.arg1 == a1);
	if (data) {
		CHECK(m.len == len);
		CHECK(!memcmp(m.data, data, len));
	}
}

static void expect_none(void)
{
	struct msg m;
	CHECK(!get(&m));
}

static uint32_t open_stream(uint32_t host, const char *svc)
{
	struct msg m;
	host_send(A_OPEN, host, 0, svc, (uint32_t)strlen(svc) + 1);
	bool ok = get(&m);
	CHECK(ok && m.cmd == A_OKAY && m.arg1 == host && m.arg0 != 0);
	return m.arg0;
}

static void connect(uint32_t maxdata)
{
	static const char host[] = "host::features=shell_v2,cmd,stat_v2";
	const char *banner = "device::" PROPS ";features=";
	host_send(A_CNXN, 0x01000001, maxdata, host, sizeof host - 1);
	expect(A_CNXN, 0x01000000, 4096, banner, (uint32_t)strlen(banner));
	CHECK(adb_online());
}

static void test_handshake(void)
{
	static const uint8_t token[20] = { 1, 2, 3 };
	CHECK(!adb_online());
	host_send(A_AUTH, 1, 0, token, sizeof token);
	expect_none();
	CHECK(!adb_online());
	connect(256 * 1024);

	uint8_t junk[24];
	memset(junk, 0x55, sizeof junk);
	adb_rx(junk, sizeof junk);
	expect_none();
	CHECK(adb_rx_expected() == 24);

	host_send(A_OPEN, 7, 0, "nope:", 6);
	expect(A_CLSE, 0, 7, NULL, 0);
	expect_none();
}

static void test_shell_exec(void)
{
	connect(16);
	uint32_t dev = open_stream(9, "shell:echo the quick brown fox jumps");
	expect(A_WRTE, dev, 9, "the quick brown ", 16);
	expect_none();
	host_send(A_OKAY, 9, dev, NULL, 0);
	expect(A_WRTE, dev, 9, "fox jumps\n", 10);
	expect_none();
	host_send(A_OKAY, 9, dev, NULL, 0);
	expect(A_CLSE, dev, 9, NULL, 0);
	host_send(A_CLSE, 9, dev, NULL, 0);
	expect_none();

	busy_mode = 2;
	dev = open_stream(11, "shell:echo x");
	expect(A_WRTE, dev, 11, "x\n", 2);
	host_send(A_OKAY, 11, dev, NULL, 0);
	expect(A_CLSE, dev, 11, NULL, 0);
	busy_mode = 1;
	host_send(A_OPEN, 12, 0, "shell:echo y", 13);
	expect_none();
	busy_mode = 0;
	struct msg m;
	CHECK(get(&m) && m.cmd == A_OKAY && m.arg1 == 12);
	dev = m.arg0;
	expect(A_WRTE, dev, 12, "y\n", 2);
	host_send(A_OKAY, 12, dev, NULL, 0);
	expect(A_CLSE, dev, 12, NULL, 0);

	/* A second exec while the first waits for the main loop is refused;
	 * one the host closes before the main loop ran gives nothing. */
	host_send(A_OPEN, 13, 0, "shell:echo p", 13);
	host_send(A_OPEN, 14, 0, "shell:echo q", 13);
	CHECK(get(&m) && m.cmd == A_OKAY && m.arg1 == 13);
	dev = m.arg0;
	CHECK(get(&m) && m.cmd == A_OKAY && m.arg1 == 14);
	uint32_t dev2 = m.arg0;
	bool p = false, q = false, c13 = false, c14 = false;
	while (get(&m)) {
		if (m.cmd == A_WRTE && m.arg0 == dev && m.len == 2 && !memcmp(m.data, "p\n", 2))
			p = true;
		else if (m.cmd == A_WRTE && m.arg0 == dev2 && m.len == 5 && !memcmp(m.data, "busy\n", 5))
			q = true;
		else if (m.cmd == A_CLSE && m.arg0 == dev)
			c13 = true;
		else if (m.cmd == A_CLSE && m.arg0 == dev2)
			c14 = true;
		else
			CHECK(!"unexpected message");
		if (m.cmd == A_WRTE)
			host_send(A_OKAY, m.arg1, m.arg0, NULL, 0);
	}
	CHECK(p && q && c13 && c14);
	host_send(A_CLSE, 13, dev, NULL, 0);
	host_send(A_CLSE, 14, dev2, NULL, 0);
	expect_none();
	host_send(A_OPEN, 15, 0, "shell:echo r", 13);
	CHECK(pop(&m) && m.cmd == A_OKAY && m.arg1 == 15);
	host_send(A_CLSE, 15, m.arg0, NULL, 0);
	expect(A_CLSE, m.arg0, 15, NULL, 0);
	expect_none();
}

/* A fake host that takes everything the device sends while a command
 * runs: the pump of a transport, called from inside the console output. */
static char drained[65536];
static size_t drained_len;
static uint32_t drain_dev;
static bool drain_closed;

static void fake_pump(void)
{
	struct msg m;
	adb_poll();
	while (pop(&m)) {
		if (m.cmd == A_WRTE && m.arg0 == drain_dev) {
			CHECK(drained_len + m.len <= sizeof drained);
			memcpy(drained + drained_len, m.data, m.len);
			drained_len += m.len;
			host_send(A_OKAY, m.arg1, m.arg0, NULL, 0);
		} else if (m.cmd == A_CLSE && m.arg0 == drain_dev) {
			drain_closed = true;
		}
	}
}

static void test_long_output(void)
{
	static const struct adb_transport pumped = { fake_send, fake_pump };
	adb_init(&pumped, PROPS);
	connect(4096);
	struct msg m;
	host_send(A_OPEN, 50, 0, "shell:mem 0 1024", 17);
	CHECK(pop(&m) && m.cmd == A_OKAY && m.arg1 == 50);
	drain_dev = m.arg0;
	console_poll();
	fake_pump();
	CHECK(drain_closed);
	/* 256 lines of 46 bytes, far more than a stream's ring holds. */
	CHECK(drained_len == 256 * 46);
	CHECK(!memcmp(drained, "00000000: deadbeef deadbeeb deadbee7 deadbee3\n", 46));
	CHECK(!memcmp(drained + drained_len - 46, "00000ff0: deadb11f deadb11b deadb117 deadb113\n", 46));
	/* Without a pump the output is cut at the ring, as before. */
	adb_init(&transport, PROPS);
	connect(4096);
	drained_len = 0;
	drain_closed = false;
	host_send(A_OPEN, 51, 0, "shell:mem 0 1024", 17);
	CHECK(pop(&m) && m.cmd == A_OKAY && m.arg1 == 51);
	drain_dev = m.arg0;
	console_poll();
	fake_pump();
	CHECK(drain_closed && drained_len == 4096);
}

static void test_interactive(void)
{
	connect(4096);
	uint32_t dev = open_stream(30, "shell:");
	expect(A_WRTE, dev, 30, "> ", 2);
	host_send(A_OKAY, 30, dev, NULL, 0);
	host_send(A_WRTE, 30, dev, "echo hi\r", 8);
	expect(A_OKAY, dev, 30, NULL, 0);
	expect(A_WRTE, dev, 30, "echo hi\r\nhi\r\n> ", 15);
	host_send(A_OKAY, 30, dev, NULL, 0);
	CHECK(open_stream(31, "shell:echo z") != 0);
	host_send(A_OPEN, 32, 0, "shell:", 7);
	struct msg m;
	bool closed = false;
	while (get(&m))
		if (m.cmd == A_CLSE && m.arg0 == 0 && m.arg1 == 32)
			closed = true;
	CHECK(closed);
	host_send(A_WRTE, 30, dev, "\x04", 1);
	expect(A_OKAY, dev, 30, NULL, 0);
	expect(A_CLSE, dev, 30, NULL, 0);

	static uint8_t big[20000];
	dev = open_stream(33, "sync:");
	host_send_split(A_WRTE, 33, dev, big, sizeof big, 4096);
	expect_none();
	host_send(A_CLSE, 33, dev, NULL, 0);
	expect(A_CLSE, dev, 33, NULL, 0);
}

static uint8_t sbuf[32768];
static uint32_t slen;

static void sync_put(const char *id, uint32_t v, const void *data, uint32_t n)
{
	memcpy(sbuf + slen, id, 4);
	put32(sbuf + slen + 4, v);
	memcpy(sbuf + slen + 8, data, n);
	slen += 8 + n;
}

static void sync_request(uint32_t host, uint32_t dev, const char *id, const char *path)
{
	slen = 0;
	sync_put(id, (uint32_t)strlen(path), path, (uint32_t)strlen(path));
	host_send(A_WRTE, host, dev, sbuf, slen);
	expect(A_OKAY, dev, host, NULL, 0);
}

static uint32_t sync_reply(uint32_t host, uint32_t dev, uint8_t *buf, uint32_t max)
{
	uint32_t n = 0;
	struct msg m;
	while (get(&m)) {
		CHECK(m.cmd == A_WRTE && m.arg0 == dev && m.arg1 == host);
		CHECK(n + m.len <= max);
		memcpy(buf + n, m.data, m.len);
		n += m.len;
		host_send(A_OKAY, host, dev, NULL, 0);
	}
	return n;
}

static void test_sync(void)
{
	static uint8_t file[10240], back[16384];
	for (unsigned i = 0; i < sizeof file; i++)
		file[i] = (uint8_t)(i * 7 + i / 251);
	connect(4096);
	uint32_t dev = open_stream(20, "sync:");

	const char *spec = "/tmp/f,33188";
	slen = 0;
	sync_put("SEND", (uint32_t)strlen(spec), spec, (uint32_t)strlen(spec));
	sync_put("DATA", 6000, file, 6000);
	sync_put("DATA", sizeof file - 6000, file + 6000, sizeof file - 6000);
	sync_put("DONE", 1234, NULL, 0);
	for (uint32_t off = 0; off < slen; off += 1000) {
		uint32_t k = slen - off < 1000 ? slen - off : 1000;
		host_send_split(A_WRTE, 20, dev, sbuf + off, k, 333);
		expect(A_OKAY, dev, 20, NULL, 0);
		if (off + k < slen)
			expect_none();
	}
	expect(A_WRTE, dev, 20, "OKAY\0\0\0\0", 8);
	host_send(A_OKAY, 20, dev, NULL, 0);
	expect_none();

	uint8_t st[16];
	sync_request(20, dev, "STAT", "/nope");
	CHECK(sync_reply(20, dev, back, sizeof back) == 16);
	memcpy(st, "STAT", 4);
	memset(st + 4, 0, 12);
	CHECK(!memcmp(back, st, 16));
	sync_request(20, dev, "STAT", "/tmp/f");
	CHECK(sync_reply(20, dev, back, sizeof back) == 16);
	put32(st + 4, 33188);
	put32(st + 8, sizeof file);
	put32(st + 12, 1234);
	CHECK(!memcmp(back, st, 16));

	sync_request(20, dev, "RECV", "/tmp/f");
	uint32_t n = sync_reply(20, dev, back, sizeof back), got = 0, chunks = 0;
	bool done = false;
	for (uint32_t i = 0; i + 8 <= n && !done;) {
		uint32_t id = get32(back + i), len = get32(back + i + 4);
		i += 8;
		if (id == ID("DONE"))
			done = i == n;
		else {
			CHECK(id == ID("DATA") && len <= 4096 && i + len <= n);
			CHECK(!memcmp(back + i, file + got, len));
			got += len;
			i += len;
			chunks++;
		}
	}
	CHECK(done && got == sizeof file && chunks >= 3);

	sync_request(20, dev, "LIST", "/tmp");
	n = sync_reply(20, dev, back, sizeof back);
	CHECK(n == 21 + 20);
	CHECK(get32(back) == ID("DENT") && get32(back + 4) == 33188 && get32(back + 8) == sizeof file);
	CHECK(get32(back + 12) == 1234 && get32(back + 16) == 1 && back[20] == 'f');
	CHECK(get32(back + 21) == ID("DONE"));

	sync_request(20, dev, "RECV", "/missing");
	n = sync_reply(20, dev, back, sizeof back);
	CHECK(n > 8 && get32(back) == ID("FAIL") && get32(back + 4) == n - 8);

	slen = 0;
	sync_put("QUIT", 0, NULL, 0);
	host_send(A_WRTE, 20, dev, sbuf, slen);
	expect(A_OKAY, dev, 20, NULL, 0);
	expect(A_CLSE, dev, 20, NULL, 0);
	expect_none();

	const struct adb_file *f = adb_fs_find("/tmp/f");
	CHECK(f && f->size == sizeof file && !memcmp(f->data, file, sizeof file));
}

static void test_reboot(void)
{
	jmp_buf jb;
	volatile bool rebooted = false;
	connect(4096);
	if (setjmp(jb))
		rebooted = true;
	else {
		stub_reboot_jmp = &jb;
		host_send(A_OPEN, 40, 0, "reboot:bootloader", 18);
		pump();
	}
	stub_reboot_jmp = NULL;
	CHECK(rebooted);
	adb_reset();
	CHECK(!adb_online());
	struct msg m;
	CHECK(pop(&m) && m.cmd == A_OKAY && m.arg1 == 40);
	uint32_t dev = m.arg0;
	CHECK(pop(&m) && m.cmd == A_CLSE && m.arg0 == dev && m.arg1 == 40);
	CHECK(!pop(&m));
}

int main(void)
{
	console_init();
	adb_init(&transport, PROPS);
	test_handshake();
	test_shell_exec();
	test_long_output();
	test_interactive();
	test_sync();
	test_reboot();
	printf("adb_test: %s\n", stub_failures ? "FAILED" : "ok");
	return stub_failures != 0;
}
