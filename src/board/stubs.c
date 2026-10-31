/* Stand-ins for the drivers a board does not have (yet): each probes
 * as absent, so the main loop skips it. A board's own driver overrides
 * them by defining the strong symbol. */
#include "asteroid.h"
#include "input/touch.h"
#include "sensors/hub.h"

#define WEAK __attribute__((weak))

WEAK bool touch_init(void) { return false; }
WEAK bool touch_poll(struct touch *t) { return false; }
WEAK void touch_stats(uint32_t *attentions, uint32_t *errors) { *attentions = *errors = 0; }
WEAK void touch_inject(int x0, int y0, int x1, int y1, uint32_t ms) {}

WEAK bool haptics_init(void) { return false; }
WEAK void haptics_play(const uint16_t *on_off_ms, unsigned n) {}
WEAK void haptics_poll(void) {}
WEAK bool haptics_busy(void) { return false; }

WEAK bool hub_init(void) { return false; }
WEAK void hub_poll(void) {}
WEAK int hub_send(const uint8_t *p, unsigned n) { return -1; }
WEAK bool hub_tilted(void) { return false; }
WEAK bool hub_can_sleep(void) { return true; }
WEAK void hub_sleep(bool asleep) {}
WEAK void hub_status(void (*say)(const char *fmt, ...)) { say("hub: no driver on this watch\n"); }

WEAK int speaker_probe(uint16_t *rev) { return -1; }
