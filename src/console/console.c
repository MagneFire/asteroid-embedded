/* Line-oriented command console. An attached terminal feeds it raw bytes
 * that are edited into a line and run on return; with echo on, the
 * terminal is taken to be in raw mode, so output newlines become CRLF and
 * a prompt is shown. console_exec runs a single line for a caller with
 * its own output. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "bt/bt.h"
#include "storage/storage.h"
#include "sensors/hub.h"
#include "input/touch.h"
#include "screenshot/screenshot.h"
#include "notify/notify.h"
#include "media/media.h"
#include "audio/speaker.h"
#include "ui/ui.h"
#include "console.h"

#define LINE_LEN	128
#define PROMPT		"> "

typedef void (*out_fn)(const char *s, size_t n, void *ctx);

struct command {
	const char *name, *usage;
	void (*fn)(const char *args);
};

static char line[LINE_LEN];
static size_t line_len;
static bool last_cr;
static out_fn term_out;
static void *term_ctx;
static bool term_echo;

static out_fn cur_out;
static void *cur_ctx;
static bool cur_crlf;

/* Lines arrive from the USB stack (CDC and adb callbacks), which also
 * runs under the display's idle hook, so a command is not run where its
 * line arrives: it waits in this slot until console_poll runs it from the
 * main loop. A terminal line uses the terminal sink of that moment, so a
 * shell that closes in between gets nothing; an exec job keeps its own
 * sink and tells its owner when it is done. */
static struct {
	bool pending, terminal;
	char line[LINE_LEN];
	out_fn out;
	void *ctx;
	void (*done)(void *ctx);
} job;
static bool running;

static void emit_to(out_fn out, void *ctx, bool crlf, const char *s, size_t n)
{
	if (!out)
		return;
	if (!crlf) {
		out(s, n, ctx);
		return;
	}
	while (n) {
		size_t k = 0;
		while (k < n && s[k] != '\n')
			k++;
		if (k)
			out(s, k, ctx);
		if (k == n)
			break;
		out("\r\n", 2, ctx);
		s += k + 1;
		n -= k + 1;
	}
}

/* Command output goes to the sink of the command being run. */
static void emit(const char *s, size_t n)
{
	emit_to(cur_out, cur_ctx, cur_crlf, s, n);
}

/* Echo and prompts go to the terminal, whatever is running: input can
 * arrive while a command runs from the main loop. */
static void term_emit(const char *s, size_t n)
{
	emit_to(term_out, term_ctx, term_echo, s, n);
}

static void say(const char *fmt, ...)
{
	char buf[192];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof buf - 1)
		n = (int)sizeof buf - 1;
	if (n > 0)
		emit(buf, (size_t)n);
}

static const char *skip_spaces(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

static bool parse_num(const char **p, int base, uint32_t *v)
{
	const char *s = skip_spaces(*p);
	char *end;
	if (!*s)
		return false;
	*v = (uint32_t)strtoul(s, &end, base);
	if (end == s || (*end && *end != ' ' && *end != '\t'))
		return false;
	*p = end;
	return true;
}

static void cmd_help(const char *args);

static void cmd_status(const char *args)
{
	uint32_t ms = time_ms();
	say("uptime %lu.%03lu s\n", ms / 1000, ms % 1000);
	say("build %s\n", build_id);
	say("timer ticks %lu\n", timer_ticks());
	say("display frames %lu\n", display_frames());
	say("battery %lu mV\n", pmic_battery_mv());
	say("charger %s\n", pmic_charger_status());
	say("rtc %lu\n", pmic_rtc_read());
}

static void log_out(const char *s, size_t n, void *ctx)
{
	emit(s, n);
}

static void cmd_log(const char *args)
{
	uint32_t n = 20;
	if (*args && !parse_num(&args, 10, &n)) {
		say("usage: log [lines] [pattern]\n");
		return;
	}
	while (*args == ' ')
		args++;
	if (*args)
		log_grep(args, n, log_out, NULL);
	else
		log_tail(n, log_out, NULL);
}

static void cmd_reboot(const char *args)
{
	say("rebooting\n");
	reboot();
}

static void cmd_bright(const char *args)
{
	uint32_t v;
	if (!parse_num(&args, 10, &v) || v > 255) {
		say("usage: bright <0-255>\n");
		return;
	}
	display_set_brightness((uint8_t)v);
}

static void cmd_sleep(const char *args) { power_request(true); }
static void cmd_wake(const char *args) { power_request(false); }

static void cmd_set(const char *args)
{
	bool found = false;
	for (unsigned i = 0; i < SETTINGS; i++) {
		const char *name = setting_name((enum setting)i);
		size_t n = strlen(name);
		uint32_t v;
		if (*args && (strncmp(args, name, n) || (args[n] && args[n] != ' ')))
			continue;
		const char *p = args + n;
		if (*args && *p) {
			if (!parse_num(&p, 10, &v) || v > 255) {
				say("usage: set [name [value]]\n");
				return;
			}
			setting_set((enum setting)i, (int)v);
			settings_flush();
			settings_changed();
		}
		say("%s %d\n", name, setting_get((enum setting)i));
		found = true;
	}
	if (!found)
		say("set: no setting %s\n", args);
}

static void cmd_aod(const char *args)
{
	if (!strcmp(args, "on") || !strcmp(args, "off")) {
		setting_set(SETTING_AOD, !strcmp(args, "on"));
		settings_flush();
		settings_changed();
	} else if (*args)
		say("usage: aod [on|off]\n");
	say("aod %s\n", setting_get(SETTING_AOD) ? "on" : "off");
}

static void cmd_rtc(const char *args)
{
	uint32_t v;
	if (*args) {
		if (!parse_num(&args, 10, &v)) {
			say("usage: rtc [seconds]\n");
			return;
		}
		pmic_rtc_write(v);
	}
	say("rtc %lu\n", pmic_rtc_read());
}

static void cmd_mem(const char *args)
{
	uint32_t addr, words = 1;
	if (!parse_num(&args, 16, &addr) || (*skip_spaces(args) && !parse_num(&args, 0, &words))) {
		say("usage: mem <hex addr> [words]\n");
		return;
	}
	addr &= ~3u;
	if (words > 1024)
		words = 1024;
	for (uint32_t i = 0; i < words; i++) {
		if (i % 4 == 0)
			say("%08lx:", addr + i * 4);
		say(" %08lx", readl(addr + i * 4));
		if (i % 4 == 3 || i + 1 == words)
			say("\n");
	}
}

static void cmd_pmic(const char *args)
{
	uint32_t addr, count = 1;
	if (!parse_num(&args, 16, &addr) || (*skip_spaces(args) && !parse_num(&args, 0, &count))) {
		say("usage: pmic <hex addr> [bytes]\n");
		return;
	}
	if (count > 256)
		count = 256;
	for (uint32_t i = 0; i < count; i++) {
		if (i % 16 == 0)
			say("%05lx:", addr + i);
		say(" %02x", pmic_read(addr + i));
		if (i % 16 == 15 || i + 1 == count)
			say("\n");
	}
}

static bool parse_bdaddr(const char *s, uint8_t a[6])
{
	for (int i = 0; i < 6; i++) {
		char *end;
		unsigned long v = strtoul(s, &end, 16);
		if (end == s || end - s > 2 || v > 0xff || (i < 5 ? *end != ':' : *end != 0))
			return false;
		a[i] = (uint8_t)v;
		s = end + 1;
	}
	return true;
}

static void say_addr(const char *what, const uint8_t *a, unsigned type)
{
	say("%s%02x:%02x:%02x:%02x:%02x:%02x (type %u)\n", what, a[5], a[4], a[3], a[2], a[1], a[0], type);
}

static void bt_status_say(void)
{
	static const char *const states[] = { "off", "starting", "advertising", "connected", "failed to sync" };
	struct bt_status s;
	bt_host_status(&s);
	say("bt: chip %s, host %s%s, %lu resets\n", bt_running() ? "up" : "off", states[s.state],
	    s.synced ? " (synced)" : "", s.resets);
	if (s.connected) {
		say_addr("peer ", s.peer, s.peer_type);
		say("encrypted %u, authenticated %u, bonded %u, interval %u x 1.25 ms, mtu %u\n", s.encrypted,
		    s.authenticated, s.bonded, s.interval, s.mtu);
	}
	say("bonded peers %u, key store \"%s\"\n", s.bonds, s.kv);
	for (unsigned i = 0; i < s.bonds && i < ARRAY_SIZE(s.bond_addr); i++)
		say_addr("  ", s.bond_addr[i], s.bond_type[i]);
	say("hci %lu commands, %lu events, acl %lu out, %lu in, %lu dropped, %lu tx errors\n", s.transport.commands,
	    s.transport.events, s.transport.acl_out, s.transport.acl_in, s.transport.dropped, s.transport.tx_errors);
	if (s.next_ms != UINT32_MAX)
		say("next host timer in %lu ms\n", s.next_ms);
}

static void cmd_bt(const char *args)
{
	uint8_t addr[6];
	bool have_addr = false;
	struct bt_info info;
	if (!strcmp(args, "status")) {
		bt_status_say();
		return;
	}
	if (!strcmp(args, "forget")) {
		say(bt_store_clear() ? "bt: could not clear the bonds\n" : "bt: bonds cleared\n");
		return;
	}
	if (!strcmp(args, "off")) {
		bt_host_stop();
		bt_stop();
		say("bt off\n");
		return;
	}
	if (*args) {
		if (!parse_bdaddr(args, addr)) {
			say("usage: bt [off | status | forget | <bd_addr>]\n");
			return;
		}
		have_addr = true;
		if (bt_addr_save(addr))
			say("bt: could not store the address\n");
	} else {
		have_addr = !bt_addr_load(addr);
	}
	bt_host_stop();
	if (bt_start(have_addr ? addr : NULL, &info)) {
		say("bt: %s failed (%d)\n", info.failed, info.status);
		return;
	}
	say("bt: up in %lu ms, patch %lu ms\n", info.total_ms, info.patch_ms);
	say("hci %u rev %04x, lmp %u subversion %04x, manufacturer %u\n", info.hci_version, info.hci_revision,
	    info.lmp_version, info.lmp_subversion, info.manufacturer);
	say("bd_addr %02x:%02x:%02x:%02x:%02x:%02x, name \"%s\", host wake %u\n", info.bdaddr[0], info.bdaddr[1],
	    info.bdaddr[2], info.bdaddr[3], info.bdaddr[4], info.bdaddr[5], info.name, bt_host_wake());
	int r = bt_host_start();
	if (r)
		say("bt: host failed (%d)\n", r);
	else
		say("bt: host starting, advertises once synced (bt status)\n");
}

static void hexdump(uint32_t base, const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 16) {
		char hex[16 * 3 + 1], asc[17];
		for (uint32_t j = 0; j < 16; j++) {
			uint8_t c = i + j < n ? p[i + j] : 0;
			snprintf(hex + j * 3, 4, i + j < n ? " %02x" : "   ", c);
			asc[j] = i + j < n ? (c >= 0x20 && c < 0x7f ? (char)c : '.') : ' ';
		}
		asc[16] = 0;
		say("%08lx:%s  %s\n", base + i, hex, asc);
	}
}

static uint32_t bits(const uint32_t *r, unsigned start, unsigned len)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < len; i++)
		v |= ((r[(start + i) / 32] >> ((start + i) % 32)) & 1u) << i;
	return v;
}

static void cmd_emmc(const char *args)
{
	int r = emmc_init(strcmp(args, "safe") != 0);
	const struct emmc_info *c = emmc_card();
	say("host: mci %08lx caps %08lx, %u-bit, %lu Hz%s\n", c->mci_version, c->caps, c->bus_width, c->clock_hz,
	    c->high_speed ? ", HS" : "");
	if (r) {
		say("init failed: %d (log has details)\n", r);
		return;
	}
	char pnm[7];
	for (int i = 0; i < 6; i++)
		pnm[i] = (char)bits(c->cid, 96 - i * 8, 8);
	pnm[6] = 0;
	const uint8_t *e = c->ext_csd;
	say("cid: mid %02lx oid %02lx name %s rev %lu.%lu serial %08lx date %lu/%lu\n", bits(c->cid, 120, 8), bits(c->cid, 104, 8),
	    pnm, bits(c->cid, 52, 4), bits(c->cid, 48, 4), bits(c->cid, 16, 32), bits(c->cid, 12, 4), bits(c->cid, 8, 4));
	say("csd: structure %lu spec %lu tran_speed %02lx\n", bits(c->csd, 126, 2), bits(c->csd, 122, 4), bits(c->csd, 96, 8));
	say("ocr %08lx, %lu sectors = %lu MiB\n", c->ocr, c->sectors, c->sectors / 2048);
	say("ext_csd: rev %u card_type %02x hs_timing %u bus_width %u part_config %02x\n", e[192], e[196], e[185], e[183], e[179]);
	say("ext_csd: boot %u x 128 KiB, rpmb %u x 128 KiB, rst_n %u, wr_rel %02x/%02x, erase_grp_def %u\n", e[226], e[168], e[162],
	    e[166], e[167], e[175]);
	say("ext_csd: pre_eol %u life %u/%u, fw %02x%02x%02x%02x%02x%02x%02x%02x\n", e[267], e[268], e[269], e[254], e[255], e[256],
	    e[257], e[258], e[259], e[260], e[261]);
}

static bool storage_ready(void)
{
	const struct emmc_info *c = emmc_card();
	int r = 0;
	if (!c->ready && (r = emmc_init(true))) {
		say("emmc init failed: %d\n", r);
		return false;
	}
	if (!emmc_part_count() && (r = emmc_part_scan())) {
		say("no usable GPT: %d\n", r);
		return false;
	}
	return true;
}

static void cmd_part(const char *args)
{
	if (!storage_ready())
		return;
	for (unsigned i = 0; i < emmc_part_count(); i++) {
		const struct emmc_part *p = emmc_part_get(i);
		say("%2u %-12s %8lu %8lu %7lu KiB\n", i + 1, p->name, p->first, p->count, p->count / 2);
	}
}

static void cmd_emmcrd(const char *args)
{
	char name[37];
	unsigned n = 0;
	args = skip_spaces(args);
	while (*args && *args != ' ' && *args != '\t' && n < sizeof name - 1)
		name[n++] = *args++;
	name[n] = 0;
	uint32_t off, count = 1, first, size;
	if (!n || !parse_num(&args, 0, &off) || (*skip_spaces(args) && !parse_num(&args, 0, &count)) || !count || count > 8) {
		say("usage: emmcrd <partition> <sector> [count 1-8]\n");
		return;
	}
	if (!storage_ready())
		return;
	if (emmc_part_find(name, &first, &size)) {
		say("no partition %s\n", name);
		return;
	}
	if (off >= size || count > size - off) {
		say("outside %s (%lu sectors)\n", name, size);
		return;
	}
	static uint8_t buf[8 * EMMC_BLOCK];
	int r = emmc_read(first + off, count, buf);
	if (r) {
		say("read failed: %d\n", r);
		return;
	}
	hexdump(off * EMMC_BLOCK, buf, count * EMMC_BLOCK);
}

static void ls_fn(const char *name, unsigned len, uint32_t ino, unsigned type, void *ctx)
{
	static const char types[] = "?-dcbpsl";
	say("%c %8lu %s\n", type < 8 ? types[type] : '?', ino, name);
}

static struct ext4 *lookup(const char *args, struct ext4_inode *in)
{
	const char *rest;
	struct ext4 *fs = storage_resolve(skip_spaces(args), &rest);
	if (!fs) {
		say("storage not mounted (see log)\n");
		return NULL;
	}
	int r = ext4_lookup(fs, rest, in);
	if (r) {
		say("%s: %s\n", rest, ext4_strerror(r));
		return NULL;
	}
	return fs;
}

static void cmd_ls(const char *args)
{
	struct ext4_inode in;
	struct ext4 *fs = lookup(args, &in);
	if (!fs)
		return;
	if ((in.mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
		say("%06o %lu bytes, flags %08lx, inode %lu\n", in.mode, (uint32_t)in.size, in.flags, in.ino);
		return;
	}
	int r = ext4_list(fs, &in, ls_fn, NULL);
	if (r)
		say("%s\n", ext4_strerror(r));
}

static void cmd_cat(const char *args)
{
	struct ext4_inode in;
	struct ext4 *fs = lookup(args, &in);
	if (!fs)
		return;
	static char buf[2048];
	int r = ext4_read(fs, &in, 0, buf, sizeof buf - 1);
	if (r < 0) {
		say("%s\n", ext4_strerror(r));
		return;
	}
	for (int i = 0; i < r; i++)
		if ((buf[i] < 0x20 || buf[i] >= 0x7f) && buf[i] != '\n' && buf[i] != '\t')
			buf[i] = '.';
	buf[r] = 0;
	for (int i = 0; i < r; i += 160)
		say("%.160s", buf + i);
	if (r && buf[r - 1] != '\n')
		say("\n");
	if (in.size > (uint64_t)r)
		say("[%lu of %lu bytes]\n", (uint32_t)r, (uint32_t)in.size);
}

static void cmd_fs(const char *args)
{
	if (storage_mount()) {
		say("storage not mounted (see log)\n");
		return;
	}
	struct ext4 *v[2] = { storage_userdata(), storage_rootfs() };
	for (int i = 0; i < 2; i++) {
		if (i && v[1] == v[0])
			break;
		struct ext4 *fs = v[i];
		say("%s: label '%s' bs %lu blocks %lu inodes %lu isize %lu desc %lu state %04x\n", fs->name, fs->label, fs->bs,
		    (uint32_t)fs->blocks, fs->inodes, fs->inode_size, fs->desc_size, fs->state);
		say("  compat %08lx incompat %08lx ro_compat %08lx mount_opts %08lx%s\n", fs->compat, fs->incompat, fs->ro_compat,
		    fs->default_mount_opts, (fs->incompat & EXT4_INCOMPAT_RECOVER) ? " NEEDS RECOVERY" : "");
	}
	say("AsteroidOS root: %s\n", storage_rootfs_where());
}

static void cmd_store(const char *args)
{
	int r = store_open();
	const struct store_info *s = store_info();
	if (r) {
		say("no store (%s): %s\n", STORE_PARTITION, s->why ? s->why : "storage not mounted");
		return;
	}
	say("%lu bytes, %s%s\n", s->size, s->writable ? "writable" : "read-only: ", s->why ? s->why : "");
	for (unsigned i = 0; i < s->nsegs; i++)
		say("  offset %7lu: lba %8lu + %lu\n", s->segs[i].off, s->segs[i].lba, s->segs[i].count);
}

static void cmd_emmctest(const char *args)
{
	if (store_open() || !store_info()->writable) {
		say("store not writable\n");
		return;
	}
	static uint8_t save[4096], pat[4096], back[4096];
	uint32_t off = store_info()->size - sizeof save;
	int r = store_read(off, save, sizeof save);
	for (uint32_t i = 0; i < sizeof pat; i++)
		pat[i] = (uint8_t)(i * 7 + time_us());
	uint32_t t0 = time_us();
	if (!r)
		r = store_write(off, pat, sizeof pat);
	uint32_t t1 = time_us();
	if (!r)
		r = store_read(off, back, sizeof back);
	bool same = !r && !memcmp(pat, back, sizeof pat);
	int r2 = store_write(off, save, sizeof save);
	if (!r2)
		r2 = store_read(off, back, sizeof back);
	bool restored = !r2 && !memcmp(save, back, sizeof save);
	say("write %d (%lu us), compare %s, restore %s\n", r, t1 - t0, same ? "ok" : "FAILED", restored ? "ok" : "FAILED");
}

static void kv_fn(const char *key, const uint8_t *val, unsigned len, void *ctx)
{
	say("%s (%u bytes):", key, len);
	for (unsigned i = 0; i < len && i < 32; i++)
		say(" %02x", val[i]);
	say(len > 32 ? " ...\n" : "\n");
}

static void cmd_kv(const char *args)
{
	char op[8], key[KV_KEY_MAX + 1];
	unsigned n = 0;
	args = skip_spaces(args);
	while (*args && *args != ' ' && n < sizeof op - 1)
		op[n++] = *args++;
	op[n] = 0;
	args = skip_spaces(args);
	n = 0;
	while (*args && *args != ' ' && n < sizeof key - 1)
		key[n++] = *args++;
	key[n] = 0;
	args = skip_spaces(args);
	int r;
	if (!*op) {
		r = kv_each(kv_fn, NULL);
		if (!r)
			say("seq %lu\n", kv_seq());
	} else if (!strcmp(op, "set") && *key) {
		r = kv_set(key, args, (unsigned)strlen(args));
	} else if (!strcmp(op, "del") && *key) {
		r = kv_del(key);
	} else {
		say("usage: kv [set <key> <text> | del <key>]\n");
		return;
	}
	if (r)
		say("kv: %d\n", r);
}

struct btscan { struct ext4 *fs; char dir[96]; };

static void bt_peer_fn(const char *name, unsigned len, uint32_t ino, unsigned type, void *ctx)
{
	struct btscan *b = ctx;
	if (len != 17 || name[2] != ':')
		return;
	char path[128];
	snprintf(path, sizeof path, "%s/%s/info", b->dir, name);
	struct ext4_inode in;
	static char info[1536];
	if (ext4_lookup(b->fs, path, &in)) {
		say("  %s: no info\n", name);
		return;
	}
	int r = ext4_read(b->fs, &in, 0, info, sizeof info - 1);
	if (r < 0)
		return;
	info[r] = 0;
	static const char *groups[] = { "[LongTermKey]", "[PeripheralLongTermKey]", "[SlaveLongTermKey]", "[IdentityResolvingKey]",
					"[LocalSignatureKey]", "[RemoteSignatureKey]", "[LinkKey]" };
	say("  %s:", name);
	for (unsigned i = 0; i < ARRAY_SIZE(groups); i++) {
		size_t gl = strlen(groups[i]);
		for (const char *p = info; *p; p++)
			if (!strncmp(p, groups[i], gl)) {
				char g[32];
				memcpy(g, groups[i] + 1, gl - 2);
				g[gl - 2] = 0;
				say(" %s", g);
				break;
			}
	}
	const char *t = info;
	while (*t && strncmp(t, "AddressType=", 12))
		t++;
	if (*t) {
		char a[24];
		unsigned k = 0;
		while (t[k] && t[k] != '\n' && k < sizeof a - 1) {
			a[k] = t[k];
			k++;
		}
		a[k] = 0;
		say(" %s", a);
	}
	say("\n");
}

static void bt_adapter_fn(const char *name, unsigned len, uint32_t ino, unsigned type, void *ctx)
{
	struct btscan *b = ctx;
	if (len != 17 || name[2] != ':')
		return;
	struct btscan peers = { b->fs, "" };
	snprintf(peers.dir, sizeof peers.dir, "/var/lib/bluetooth/%s", name);
	say("adapter %s\n", name);
	struct ext4_inode in;
	if (!ext4_lookup(b->fs, peers.dir, &in))
		ext4_list(b->fs, &in, bt_peer_fn, &peers);
}

static void cmd_btinfo(const char *args)
{
	struct ext4_inode in;
	struct ext4 *fs = lookup("/var/lib/bluetooth-rfkill-event/bluetooth_address", &in);
	if (fs) {
		char addr[24] = { 0 };
		int r = ext4_read(fs, &in, 0, addr, 17);
		say("bluetooth_address: %s\n", r == 17 ? addr : "unreadable");
	}
	fs = lookup("/var/lib/bluetooth", &in);
	if (!fs)
		return;
	struct btscan b = { fs, "" };
	ext4_list(fs, &in, bt_adapter_fn, &b);
}

static void cmd_touch(const char *args)
{
	uint32_t v[5] = { 0, 0, 0, 0, 0 };
	bool swipe = !strncmp(args, "swipe ", 6), tap = !strncmp(args, "tap ", 4);
	const char *p = args + (swipe ? 6 : 4);
	unsigned need = swipe ? 4 : 2, n = 0;
	while (n < need + (swipe ? 1 : 0)) {
		while (*p == ' ')
			p++;
		if (!*p || !parse_num(&p, 10, &v[n]))
			break;
		n++;
	}
	if ((!swipe && !tap) || n < need) {
		say("usage: touch tap <x> <y> | swipe <x0> <y0> <x1> <y1> [ms]\n");
		return;
	}
	if (tap)
		touch_inject((int)v[0], (int)v[1], (int)v[0], (int)v[1], 60);
	else
		touch_inject((int)v[0], (int)v[1], (int)v[2], (int)v[3], n > 4 ? v[4] : 200);
	say("touch %s\n", swipe ? "swipe" : "tap");
}

static void cmd_shot(const char *args)
{
	uint32_t n = 0;
	if (strncmp(args, "layer", 5)) {
		screenshot_to_file(-1);
	} else {
		args += 5;
		while (*args == ' ')
			args++;
		if (*args && (!parse_num(&args, 10, &n) || n >= UI_PANELS)) {
			say("usage: shot [layer [0-%u]]\n", UI_PANELS - 1);
			return;
		}
		screenshot_to_file((int)n);
	}
	say("encoding, then adb pull screen.jpg\n");
}

static void cmd_hub(const char *args)
{
	uint8_t buf[36];
	unsigned n = 0;
	if (strncmp(args, "send ", 5)) {
		hub_status(say);
		return;
	}
	args += 5;
	for (uint32_t v; n < sizeof buf && parse_num(&args, 16, &v) && v < 256; n++)
		buf[n] = (uint8_t)v;
	say("hub: send %u bytes: %d\n", n, hub_send(buf, n));
}

static void cmd_amp(const char *args)
{
	uint16_t rev;
	int r = speaker_probe(&rev);
	if (r)
		say("amp: no answer (%d)\n", r);
	else
		say("amp: revision %04x\n", rev);
}

static void cmd_buzz(const char *args)
{
	static uint16_t ms;
	uint32_t n = 300;
	if (*args && !parse_num(&args, 10, &n)) {
		say("usage: buzz [ms]\n");
		return;
	}
	ms = (uint16_t)(n > 5000 ? 5000 : n);
	haptics_play(&ms, 1);
	say("buzz %u ms\n", ms);
}

static void xml_text(char *out, size_t size, size_t *len, const char *s)
{
	for (; *s && *len + 7 < size; s++) {
		const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : NULL;
		if (!e) {
			out[(*len)++] = *s;
			continue;
		}
		memcpy(out + *len, e, strlen(e));
		*len += strlen(e);
	}
}

/* Plays the phone's part of the media service: the volume, whether it
 * plays, and the title and artist. */
static void cmd_media(const char *args)
{
	uint32_t vol, playing;
	if (!parse_num(&args, 10, &vol) || !parse_num(&args, 10, &playing)) {
		say("usage: media <volume> <playing> <title>|<artist>\n");
		return;
	}
	args = skip_spaces(args);
	const char *bar = strchr(args, '|');
	unsigned tn = bar ? (unsigned)(bar - args) : (unsigned)strlen(args);
	uint8_t v = (uint8_t)(vol > 100 ? 100 : vol), p = playing != 0;
	media_write(0x7001, (const uint8_t *)args, tn);
	media_write(0x7003, (const uint8_t *)(bar ? bar + 1 : ""), bar ? (unsigned)strlen(bar + 1) : 0);
	media_write(0x7004, &p, 1);
	media_write(0x7006, &v, 1);
}

static void cmd_notify(const char *args)
{
	static uint32_t tests;
	char msg[600];
	size_t n;
	uint32_t i;
	if (!strncmp(args, "remove", 6)) {
		args += 6;
		if (!parse_num(&args, 10, &i) || !i) {
			say("usage: notify remove <n>\n");
			return;
		}
		n = (size_t)snprintf(msg, sizeof msg, "<removed><id>-%lu</id></removed>", (unsigned long)i);
		notify_feed((const uint8_t *)msg, (unsigned)n);
		say("removed test %lu\n", (unsigned long)i);
		return;
	}
	i = ++tests;
	n = (size_t)snprintf(msg, sizeof msg,
			     "<insert><id>-%lu</id><pn>console</pn><an>Console</an><ai>ios-mail</ai>"
			     "<su>Test %lu</su><bo>", (unsigned long)i, (unsigned long)i);
	if (*args)
		xml_text(msg, sizeof msg - 32, &n, args);
	else
		n += (size_t)snprintf(msg + n, sizeof msg - n, "Test notification %lu", (unsigned long)i);
	n += (size_t)snprintf(msg + n, sizeof msg - n, "</bo><vb>normal</vb></insert>");
	notify_feed((const uint8_t *)msg, (unsigned)n);
	say("test %lu\n", (unsigned long)i);
}

static void cmd_uibench(const char *args)
{
	uint32_t p = 0, gpu = 0, n = 10;
	if (!parse_num(&args, 10, &p) || p >= UI_PANELS || !parse_num(&args, 10, &gpu) ||
	    (*skip_spaces(args) && !parse_num(&args, 10, &n)) || !n) {
		say("usage: uibench <0-%u> <0 cpu | 1 gpu> [times]\n", UI_PANELS - 1);
		return;
	}
	uint32_t t0 = time_us();
	for (uint32_t i = 0; i < n; i++)
		ui_repaint(p, gpu);
	say("panel %lu: %lu us per repaint\n", p, (time_us() - t0) / n);
}

static void cmd_echo(const char *args)
{
	say("%s\n", args);
}

static const struct command commands[] = {
	{ "help", "", cmd_help },
	{ "status", "", cmd_status },
	{ "log", "[lines]", cmd_log },
	{ "reboot", "", cmd_reboot },
	{ "bright", "<0-255>", cmd_bright },
	{ "sleep", "", cmd_sleep },
	{ "wake", "", cmd_wake },
	{ "aod", "[on|off]", cmd_aod },
	{ "set", "[name [value]]", cmd_set },
	{ "rtc", "[seconds]", cmd_rtc },
	{ "mem", "<hex addr> [words]", cmd_mem },
	{ "pmic", "<hex addr> [bytes]", cmd_pmic },
	{ "bt", "[off | status | forget | <bd_addr>]", cmd_bt },
	{ "emmc", "[safe]", cmd_emmc },
	{ "part", "", cmd_part },
	{ "emmcrd", "<partition> <sector> [count]", cmd_emmcrd },
	{ "fs", "", cmd_fs },
	{ "ls", "[userdata:]<path>", cmd_ls },
	{ "cat", "[userdata:]<path>", cmd_cat },
	{ "store", "", cmd_store },
	{ "emmctest", "", cmd_emmctest },
	{ "kv", "[set <key> <text> | del <key>]", cmd_kv },
	{ "btinfo", "", cmd_btinfo },
	{ "amp", "", cmd_amp },
	{ "buzz", "[ms]", cmd_buzz },
	{ "hub", "[send <hex bytes>]", cmd_hub },
	{ "touch", "tap <x> <y> | swipe <x0> <y0> <x1> <y1> [ms]", cmd_touch },
	{ "shot", "[layer [n]]", cmd_shot },
	{ "notify", "[text] | remove <n>", cmd_notify },
	{ "media", "<volume> <playing> <title>|<artist>", cmd_media },
	{ "uibench", "<panel> <gpu> [times]", cmd_uibench },
	{ "echo", "<text>", cmd_echo },
};

static void cmd_help(const char *args)
{
	for (unsigned i = 0; i < ARRAY_SIZE(commands); i++)
		say("%s %s\n", commands[i].name, commands[i].usage);
}

static void run(const char *text)
{
	char buf[LINE_LEN];
	size_t n = 0;
	text = skip_spaces(text);
	while (n < sizeof buf - 1 && text[n])
		n++;
	memcpy(buf, text, n);
	while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t' || buf[n - 1] == '\r' || buf[n - 1] == '\n'))
		n--;
	buf[n] = 0;
	if (!n)
		return;
	char *args = buf;
	while (*args && *args != ' ' && *args != '\t')
		args++;
	if (*args)
		*args++ = 0;
	args = (char *)skip_spaces(args);
	for (unsigned i = 0; i < ARRAY_SIZE(commands); i++)
		if (!strcmp(buf, commands[i].name)) {
			/* Some commands take seconds on the sleep clock. */
			bool slow = cpu_clock_is_slow();
			if (slow)
				cpu_clock_slow(false);
			commands[i].fn(args);
			if (slow)
				cpu_clock_slow(true);
			return;
		}
	say("unknown command: %s\n", buf);
}

static void use_terminal(void)
{
	cur_out = term_out;
	cur_ctx = term_ctx;
	cur_crlf = term_echo;
}

static void prompt(void)
{
	if (term_echo)
		term_emit(PROMPT, sizeof PROMPT - 1);
}

void console_init(void)
{
	line_len = 0;
	last_cr = false;
	term_out = NULL;
	cur_out = NULL;
	job.pending = running = false;
}

static bool busy(void)
{
	return job.pending || running;
}

static void queue(const char *text)
{
	size_t n = strlen(text);
	if (n > LINE_LEN - 1)
		n = LINE_LEN - 1;
	memcpy(job.line, text, n);
	job.line[n] = 0;
	job.pending = true;
}

void console_input(const char *buf, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		char c = buf[i];
		if (c == '\r' || c == '\n') {
			bool skip = c == '\n' && last_cr;
			last_cr = c == '\r';
			if (skip)
				continue;
			if (term_echo)
				term_emit("\n", 1);
			line[line_len] = 0;
			line_len = 0;
			if (busy()) {
				term_emit("busy\n", 5);
				prompt();
			} else {
				job.terminal = true;
				queue(line);
			}
			continue;
		}
		last_cr = false;
		if (c == 0x7f || c == 0x08) {
			if (line_len) {
				line_len--;
				if (term_echo)
					term_emit("\b \b", 3);
			}
		} else if (c == 0x03) {
			line_len = 0;
			if (term_echo)
				term_emit("^C\n", 3);
			prompt();
		} else if (c >= 0x20 && c < 0x7f && line_len < LINE_LEN - 1) {
			line[line_len++] = c;
			if (term_echo)
				term_emit(&c, 1);
		}
	}
}

bool console_exec(const char *text, out_fn out, void *ctx, void (*done)(void *ctx))
{
	if (busy())
		return false;
	job.terminal = false;
	job.out = out;
	job.ctx = ctx;
	job.done = done;
	queue(text);
	return true;
}

void console_cancel(void *ctx)
{
	if (job.terminal || job.ctx != ctx)
		return;
	if (running) {
		cur_out = NULL;
		cur_ctx = NULL;
		job.done = NULL;
	} else {
		job.pending = false;
	}
}

void console_poll(void)
{
	if (!job.pending || running)
		return;
	job.pending = false;
	running = true;
	bool term = job.terminal;
	if (term) {
		use_terminal();
	} else {
		cur_out = job.out;
		cur_ctx = job.ctx;
		cur_crlf = false;
	}
	run(job.line);
	void (*done)(void *) = job.done;
	void *ctx = job.ctx;
	running = false;
	use_terminal();
	if (term)
		prompt();
	else if (done)
		done(ctx);
}

void console_attach(out_fn out, void *ctx, bool echo)
{
	term_out = out;
	term_ctx = ctx;
	term_echo = echo;
	line_len = 0;
	last_cr = false;
	if (job.terminal)
		job.pending = false;
	if (!running)
		use_terminal();
	prompt();
}

void console_detach(void)
{
	term_out = NULL;
	term_ctx = NULL;
	term_echo = false;
	line_len = 0;
	if (job.terminal)
		job.pending = false;
	if (!running || job.terminal)
		use_terminal();
}
