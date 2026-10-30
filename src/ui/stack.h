/* The screen stack: the watchface with the notifications to its left
 * (swipe right), the weather to its right (swipe left) and the music
 * controls below it (swipe up), the opposite swipe going back, as on
 * AsteroidOS. Each screen is a Slint panel in its own layer (ui.h); the
 * stack owns where the layers are on the display and draws them on the
 * GPU: a touch that moves past the swipe threshold drags the screens
 * along (the press is cancelled so no click fires), except vertical
 * drags on a screen that scrolls, and letting go slides to the nearest
 * screen over 250 ms. stack_dim is how much to darken the background, in
 * proportion to how far the watchface is off centre. stack_touch routes
 * everything else to the panel on screen, or to the overlay while it is
 * up (a swipe there closes the notification); stack_update advances the
 * slide; stack_compose draws the visible layers and the overlay, which
 * fades in and out over 150 ms, between gpu_draw_begin and gpu_draw_end;
 * while the overlay fades out, stack_overlay_frozen says to leave its
 * layer as it is; stack_home snaps back to the watchface. */
#ifndef STACK_H
#define STACK_H
#include <stdint.h>
#include <stdbool.h>

void stack_init(uint32_t width, uint32_t height);
void stack_touch(int kind, int x, int y, uint32_t now_ms);
void stack_update(uint32_t now_ms);
void stack_compose(void);
void stack_home(void);
unsigned stack_screen(void);
bool stack_moving(void);
void stack_offset(int *x, int *y);
float stack_dim(void);
bool stack_overlay_frozen(void);
void stack_damage(unsigned panel, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
#endif
