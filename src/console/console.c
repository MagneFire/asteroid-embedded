/* Line-oriented command console. An attached terminal feeds it raw bytes
 * that are edited into a line and run on return; with echo on, the
 * terminal is taken to be in raw mode, so output newlines become CRLF and
 * a prompt is shown. console_exec runs a single line for a caller with
 * its own output. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
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
		say("usage: log [lines]\n");
		return;
	}
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

static void cmd_sleep(const char *args) { display_sleep(); }
static void cmd_wake(const char *args) { display_wake(); }

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
			commands[i].fn(args);
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
