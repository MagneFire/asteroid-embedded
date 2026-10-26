/* Slint user interface (ui/): ui_init once with a cached heap region, then per
 * frame feed touch events and ui_render over the finished background.
 * weekday 0 = Sunday, month 1-12; touch kind 0 down, 1 move, 2 up. */
#ifndef UI_H
#define UI_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define UI_HEAP_BASE 0x06000000u
#define UI_HEAP_SIZE 0x04000000u

enum ui_touch_kind { UI_TOUCH_DOWN, UI_TOUCH_MOVE, UI_TOUCH_UP };

bool ui_init(void *heap, uint32_t heap_size, uint32_t width, uint32_t height);
void ui_set_clock(uint32_t hour, uint32_t minute, uint32_t second, uint32_t weekday, uint32_t day, uint32_t month);
void ui_set_battery(uint32_t percent, bool charging);
void ui_touch(int kind, int x, int y);
void ui_render(uint32_t *buf, uint32_t stride_px);
void ui_panic(const char *msg, size_t len) __attribute__((noreturn));
#endif
