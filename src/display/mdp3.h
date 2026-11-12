#ifndef MDP3_H
#define MDP3_H
#include <stdbool.h>
#include <stdint.h>

void mdp3_splash_size(uint32_t *w, uint32_t *h);
bool mdp3_init(void);
/* Waits for the last kick; false if it timed out. */
bool mdp3_wait(void (*idle)(void), uint32_t *waited_us);
void mdp3_kick(const uint32_t *buf);
/* Sets fb's sizes and buffers from the panel's size. */
void display_fb_setup(uint32_t width, uint32_t panel_height);
void display_fb_clear(void);

#endif
