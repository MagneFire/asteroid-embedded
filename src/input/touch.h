#ifndef TOUCH_H
#define TOUCH_H
#include <stdint.h>
#include <stdbool.h>

enum touch_event { TOUCH_NONE, TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP, TOUCH_PALM };
struct touch {
	enum touch_event event;
	int x, y;
	bool down;
};

bool touch_init(void);
bool touch_poll(struct touch *t);
void touch_stats(uint32_t *attentions, uint32_t *errors);
void touch_inject(int x0, int y0, int x1, int y1, uint32_t ms);
#endif
