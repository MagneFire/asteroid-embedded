/* Slint user interface (ui/): ui_init once with a cached heap region and a
 * layer of stride_px * height premultiplied RGBA pixels (bytes R, G, B, A),
 * which it clears to transparent. Per frame, feed touch events, then
 * ui_update repaints the layer where the UI changed and returns the
 * bounding box of that change; ui_render blends the whole layer over a
 * finished XRGB8888 background on the CPU. weekday 0 = Sunday, month 1-12;
 * touch kind 0 down, 1 move, 2 up; a passkey below 0 hides the pairing
 * code. */
#ifndef UI_H
#define UI_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define UI_HEAP_BASE 0x06000000u
#define UI_HEAP_SIZE 0x04000000u
#define UI_LAYER_BASE 0x0a000000u

enum ui_touch_kind { UI_TOUCH_DOWN, UI_TOUCH_MOVE, UI_TOUCH_UP };

bool ui_init(void *heap, uint32_t heap_size, uint32_t *layer, uint32_t stride_px, uint32_t width, uint32_t height);
void ui_set_clock(uint32_t hour, uint32_t minute, uint32_t second, uint32_t weekday, uint32_t day, uint32_t month);
void ui_set_battery(uint32_t percent, bool charging);
void ui_set_passkey(int32_t passkey);
void ui_touch(int kind, int x, int y);
bool ui_update(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h);
void ui_render(uint32_t *buf, uint32_t stride_px);
void ui_panic(const char *msg, size_t len) __attribute__((noreturn));
#endif
