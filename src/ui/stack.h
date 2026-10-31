/* The screen stack: the watchface with the notifications to its left
 * (swipe right), Today to its right (swipe left), the app launcher
 * below it (swipe up) and the quick settings above it (swipe down),
 * the opposite swipe going back, as on AsteroidOS. stack_open_app
 * fades the app panel in over the stack; a drag from the left edge
 * drags the app's top page aside over the one below (ui_app_peek) and
 * goes a page back, or closes the app from its first page, one from
 * the top edge closes it. Each screen is a Slint panel in its own
 * layer (ui.h); the stack owns where the layers are on the display
 * and draws them on the GPU: a touch that moves past the swipe
 * threshold drags the screens along (the press is cancelled so no
 * click fires), except vertical drags on a screen that scrolls, and
 * letting go slides to the nearest screen over 100 ms. stack_dim is
 * how much to darken the background, in proportion to how far the
 * watchface is off centre, and stack_launcher how far the launcher is
 * in, for the mesh to take on its colours. stack_touch routes
 * everything else to the panel on screen, or to the overlay while it
 * is up (a swipe there closes the notification); stack_update
 * advances the slide; stack_compose draws the visible layers, an open
 * app over its own background (drawn by background at the app's
 * offset and alpha) and the overlay, which fades in and out over 300
 * ms, between gpu_draw_begin and gpu_draw_end; while the overlay
 * fades out, stack_overlay_frozen says to leave its layer as it is;
 * stack_home snaps back to the watchface, closing any app;
 * stack_sleep does so keeping the app, which stack_wake closes once the
 * watch has slept APP_KEEP_MS. */
#ifndef STACK_H
#define STACK_H
#include <stdint.h>
#include <stdbool.h>

void stack_init(uint32_t width, uint32_t height, const uint32_t *layers, uint32_t layer_px);
void stack_touch(int kind, int x, int y, uint32_t now_ms);
void stack_update(uint32_t now_ms);
void stack_compose(void (*background)(int x, int y, float alpha));
void stack_home(void);
void stack_sleep(uint32_t now_ms);
void stack_wake(uint32_t now_ms);
void stack_open_app(void);
void stack_set_launcher(unsigned layout);
void stack_indicators(void);
void stack_pending(bool on);
void stack_indicators_cpu(uint32_t *buf, uint32_t stride, int dx, int dy);
unsigned stack_screen(void);
bool stack_moving(void);
bool stack_animating(uint32_t now_ms);
void stack_offset(int *x, int *y);
float stack_dim(void);
float stack_launcher(void);
bool stack_overlay_frozen(void);
void stack_damage(unsigned panel, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
#endif
