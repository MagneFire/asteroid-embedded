#ifndef STUBS_H
#define STUBS_H
#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>

extern uint32_t stub_ms, stub_rtc, stub_brightness;
extern bool stub_asleep;
extern jmp_buf *stub_reboot_jmp;
extern int stub_failures;
extern char stub_notify[1024];

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c); stub_failures++; } } while (0)
#endif
