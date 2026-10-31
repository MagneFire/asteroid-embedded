/* Console commands and line editing on the host. */
#include <stdio.h>
#include <string.h>
#include "asteroid.h"
#include "console/console.h"
#include "stubs.h"

static char out[8192];
static size_t out_len;

static void collect(const char *s, size_t n, void *ctx)
{
	if (out_len + n < sizeof out) {
		memcpy(out + out_len, s, n);
		out_len += n;
		out[out_len] = 0;
	}
}

static void *done_ctx;
static void exec_done(void *ctx) { done_ctx = ctx; }

static const char *run(const char *line)
{
	out_len = 0;
	out[0] = 0;
	console_exec(line, collect, NULL, NULL);
	console_poll();
	return out;
}

int main(void)
{
	console_init();

	run("help");
	CHECK(strstr(out, "status \n"));
	CHECK(strstr(out, "mem <hex addr> [words]\n"));
	CHECK(!strcmp(run("echo hello  world"), "hello  world\n"));
	CHECK(!strcmp(run("  echo   padded  "), "padded\n"));
	CHECK(!strcmp(run(""), ""));
	CHECK(!strcmp(run("frobnicate now"), "unknown command: frobnicate\n"));

	CHECK(!strcmp(run("notify"), "test 1\n"));
	CHECK(strstr(stub_notify, "<id>-1</id>") && strstr(stub_notify, "<bo>Test notification 1</bo>"));
	CHECK(!strcmp(run("notify Fish & <chips>"), "test 2\n"));
	CHECK(strstr(stub_notify, "<bo>Fish &amp; &lt;chips&gt;</bo><vb>normal</vb></insert>"));
	CHECK(!strcmp(run("notify remove 2"), "removed test 2\n"));
	CHECK(!strcmp(stub_notify, "<removed><id>-2</id></removed>"));
	CHECK(!strcmp(run("notify remove"), "usage: notify remove <n>\n"));

	CHECK(!strncmp(run("uibench 1 1 3"), "panel 1: ", 9) && stub_repaints == 3);
	CHECK(!strncmp(run("uibench 2 0"), "panel 2: ", 9) && stub_repaints == 13);
	CHECK(!strcmp(run("uibench 9 1"), "usage: uibench <0-6> <0 cpu | 1 gpu> [times]\n"));

	CHECK(!strcmp(run("mem 1000"), "00001000: deadaeef\n"));
	CHECK(!strcmp(run("mem 0x1000 5"),
		      "00001000: deadaeef deadaeeb deadaee7 deadaee3\n00001010: deadaeff\n"));
	CHECK(!strcmp(run("mem 1003 1"), "00001000: deadaeef\n"));
	CHECK(strstr(run("mem"), "usage: mem"));

	run("status");
	CHECK(strstr(out, "uptime 12.345 s\n"));
	CHECK(strstr(out, "build hosttest\n"));
	CHECK(strstr(out, "battery 3900 mV\n"));
	CHECK(strstr(out, "charger discharging\n"));

	CHECK(!strcmp(run("log 2"), "line two\nline three\n"));
	CHECK(!strcmp(run("rtc 5000"), "rtc 5000\n"));
	CHECK(stub_rtc == 5000);
	run("bright 128");
	CHECK(stub_brightness == 128);
	CHECK(strstr(run("bright 300"), "usage"));
	run("sleep");
	CHECK(stub_asleep);
	run("wake");
	CHECK(!stub_asleep);

	out_len = 0;
	console_attach(collect, NULL, true);
	CHECK(!strcmp(out, "> "));
	out_len = 0;
	console_input("ecx\x7fho hi\r\n", 11);
	console_poll();
	CHECK(!strcmp(out, "ecx\b \bho hi\r\nhi\r\n> "));
	out_len = 0;
	/* Typed-ahead input is echoed before the earlier command runs. */
	console_input("echo a\nech", 10);
	console_poll();
	console_input("o bb\b\bc\r", 9);
	console_poll();
	CHECK(!strcmp(out, "echo a\r\necha\r\n> o bb\b \b\b \bc\r\nc\r\n> "));
	/* A second line before the first ran is refused. */
	out_len = 0;
	console_input("echo one\recho two\r", 18);
	CHECK(!strcmp(out, "echo one\r\necho two\r\nbusy\r\n> "));
	out_len = 0;
	console_poll();
	CHECK(!strcmp(out, "one\r\n> "));
	console_detach();
	out_len = 0;
	console_input("echo gone\r", 10);
	console_poll();
	CHECK(out_len == 0);
	/* A line typed on a shell that closes before it ran never runs. */
	console_attach(collect, NULL, true);
	console_input("echo late\r", 10);
	console_detach();
	console_attach(collect, NULL, true);
	out_len = 0;
	console_poll();
	CHECK(out_len == 0);
	console_detach();

	console_attach(collect, NULL, false);
	out_len = 0;
	console_input("echo raw\n", 9);
	console_poll();
	CHECK(!strcmp(out, "raw\n"));
	console_detach();

	/* An exec job waits for console_poll, is refused while one is
	 * pending, and can be cancelled before or muted while it runs. */
	out_len = 0;
	CHECK(console_exec("echo first", collect, &out_len, exec_done));
	CHECK(!console_exec("echo second", collect, &out_len, exec_done));
	CHECK(out_len == 0 && !done_ctx);
	console_poll();
	CHECK(!strcmp(out, "first\n") && done_ctx == &out_len);
	done_ctx = NULL;
	out_len = 0;
	CHECK(console_exec("echo dropped", collect, &out_len, exec_done));
	console_cancel(&out_len);
	console_poll();
	CHECK(out_len == 0 && !done_ctx);
	CHECK(console_exec("echo kept", collect, &out_len, exec_done));
	console_cancel(&done_ctx);
	console_poll();
	CHECK(!strcmp(out, "kept\n") && done_ctx == &out_len);

	printf("console_test: %s\n", stub_failures ? "FAILED" : "ok");
	return stub_failures != 0;
}
