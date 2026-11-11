/* Drawing frames and the state that goes with it. */
#ifndef FRAME_H
#define FRAME_H
#include <stdint.h>
#include <stdbool.h>

struct ui_colors;

void frame_init(bool have_ui, bool have_gpu, uint32_t start_ms);
void frame_busy(uint32_t now);
void frame_touch_dot(int x, int y);
void frame_app_colors(const struct ui_colors *c);
void frame_sleep(void);
void frame_wake(void);
void frame_draw(uint32_t now);
void frame_log(void);
#endif
