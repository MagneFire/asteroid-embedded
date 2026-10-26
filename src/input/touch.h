#ifndef TOUCH_H
#define TOUCH_H
#include <stdint.h>
#include <stdbool.h>

enum touch_event { TOUCH_NONE, TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP };
struct touch {
	enum touch_event event;
	int x, y;
	bool down;
};

bool touch_init(void);
bool touch_poll(struct touch *t);
void touch_stats(uint32_t *attentions, uint32_t *errors);
#endif
