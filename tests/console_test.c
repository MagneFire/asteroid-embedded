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

static const char *run(const char *line)
{
	out_len = 0;
	out[0] = 0;
	console_exec(line, collect, NULL);
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
	CHECK(!strcmp(out, "ecx\b \bho hi\r\nhi\r\n> "));
	out_len = 0;
	console_input("echo a\nech", 10);
	console_input("o bb\b\bc\r", 9);
	CHECK(!strcmp(out, "echo a\r\na\r\n> echo bb\b \b\b \bc\r\nc\r\n> "));
	console_detach();
	out_len = 0;
	console_input("echo gone\r", 10);
	CHECK(out_len == 0);

	console_attach(collect, NULL, false);
	out_len = 0;
	console_input("echo raw\n", 9);
	CHECK(!strcmp(out, "raw\n"));
	console_detach();

	printf("console_test: %s\n", stub_failures ? "FAILED" : "ok");
	return stub_failures != 0;
}
