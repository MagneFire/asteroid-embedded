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
/* Leaves what is on the panel there for a while, for a test picture. */
void frame_hold(uint32_t ms);
void frame_draw(uint32_t now);
/* Without layers: draws the always-on face, the waiting-notification
 * indicator moved by (dx, dy). */
void frame_ambient(int dx, int dy);
/* How long the last frame_draw slept with nothing to draw. */
uint32_t frame_waited_us(void);
void frame_log(void);
#ifdef SGX_BASE
/* Where the content of damaged layer bands comes from: a scan of their
 * pixels, what was painted, or both, counting where they disagree. */
enum { FRAME_BANDS_SCAN, FRAME_BANDS_PAINTED, FRAME_BANDS_VERIFY };
void frame_bands(int mode);
int frame_bands_now(void);
/* Keeps the FlatMesh's vertices while the picture stays. */
void frame_mesh_reuse(bool on);
bool frame_mesh_reuse_on(void);
#endif
#endif
