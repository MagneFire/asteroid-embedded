#ifndef CONSOLE_H
#define CONSOLE_H
#include <stddef.h>
#include <stdbool.h>

void console_init(void);
void console_input(const char *buf, size_t n);
bool console_exec(const char *line, void (*out)(const char *s, size_t n, void *ctx), size_t (*room)(void *ctx),
		  void *ctx, void (*done)(void *ctx));
void console_cancel(void *ctx);
void console_attach(void (*out)(const char *s, size_t n, void *ctx), size_t (*room)(void *ctx), void *ctx,
		    bool echo);
void console_detach(void);
void console_poll(void);
#endif
