/* Line-oriented command console. An attached terminal feeds it raw bytes
 * that are edited into a line and run on return; with echo on, the
 * terminal is taken to be in raw mode, so output newlines become CRLF and
 * a prompt is shown. console_exec runs a single line for a caller with
 * its own output. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "bt/bt.h"
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

static void emit(const char *s, size_t n)
{
	if (!cur_out)
		return;
	if (!cur_crlf) {
		cur_out(s, n, cur_ctx);
		return;
	}
	while (n) {
		size_t k = 0;
		while (k < n && s[k] != '\n')
			k++;
		if (k)
			cur_out(s, k, cur_ctx);
		if (k == n)
			break;
		cur_out("\r\n", 2, cur_ctx);
		s += k + 1;
		n -= k + 1;
	}
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
	if (!strcmp(args, "off")) {
		bt_host_stop();
		bt_stop();
		say("bt off\n");
		return;
	}
	if (*args) {
		if (!parse_bdaddr(args, addr)) {
			say("usage: bt [off | status | <bd_addr>]\n");
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
	{ "rtc", "[seconds]", cmd_rtc },
	{ "mem", "<hex addr> [words]", cmd_mem },
	{ "pmic", "<hex addr> [bytes]", cmd_pmic },
	{ "bt", "[off | status | <bd_addr>]", cmd_bt },
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
		emit(PROMPT, sizeof PROMPT - 1);
}

void console_init(void)
{
	line_len = 0;
	last_cr = false;
	term_out = NULL;
	cur_out = NULL;
}

void console_input(const char *buf, size_t n)
{
	use_terminal();
	for (size_t i = 0; i < n; i++) {
		char c = buf[i];
		if (c == '\r' || c == '\n') {
			bool skip = c == '\n' && last_cr;
			last_cr = c == '\r';
			if (skip)
				continue;
			if (term_echo)
				emit("\n", 1);
			line[line_len] = 0;
			line_len = 0;
			run(line);
			use_terminal();
			prompt();
			continue;
		}
		last_cr = false;
		if (c == 0x7f || c == 0x08) {
			if (line_len) {
				line_len--;
				if (term_echo)
					emit("\b \b", 3);
			}
		} else if (c == 0x03) {
			line_len = 0;
			if (term_echo)
				emit("^C\n", 3);
			prompt();
		} else if (c >= 0x20 && c < 0x7f && line_len < LINE_LEN - 1) {
			line[line_len++] = c;
			if (term_echo)
				emit(&c, 1);
		}
	}
}

void console_exec(const char *text, out_fn out, void *ctx)
{
	out_fn o = cur_out;
	void *x = cur_ctx;
	bool crlf = cur_crlf;
	cur_out = out;
	cur_ctx = ctx;
	cur_crlf = false;
	run(text);
	cur_out = o;
	cur_ctx = x;
	cur_crlf = crlf;
}

void console_attach(out_fn out, void *ctx, bool echo)
{
	term_out = out;
	term_ctx = ctx;
	term_echo = echo;
	line_len = 0;
	last_cr = false;
	use_terminal();
	prompt();
}

void console_detach(void)
{
	term_out = NULL;
	term_ctx = NULL;
	term_echo = false;
	line_len = 0;
	use_terminal();
}

void console_poll(void)
{
}
