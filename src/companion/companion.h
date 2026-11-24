/* Companion mode: the Apollo3 next to beluga's application processor,
 * answering its framework messages as the stock firmware does. In the
 * normal mode the AP has the panel and the touch controller; in the
 * wristband mode the watch here has them, and after a 374 nobody does:
 * the watch stays dark until the AP is back. Until the AP says
 * otherwise the watch runs on its own, but for a reset other than a
 * power-on in the normal mode, or any reset by the AP: then the AP
 * keeps them. */
#ifndef COMPANION_H
#define COMPANION_H
#include <stdbool.h>
#include <stdint.h>

/* Before the pads are set and the display and touch start: the stored
 * mode, and whether the AP still has them from before a reset, in which
 * case the pads and buses it shares stay alone. */
bool companion_boot(void);
void companion_init(void);
/* Once a pass of the main loop: the messages and the mode changes. */
void companion_pass(void);
/* The link alone, also from the display's waits. */
void companion_poll(void);
uint32_t companion_next_ms(void);
/* How long until a long flash write may go, 0 when it may now. */
uint32_t companion_store_wait_ms(void);
/* No transfer under way, so the core may sleep deeply. */
bool companion_resting(void);
/* Before a reset: the mode and the zone go to the store without the
 * wait for a quiet link. */
void companion_flush(void);
void companion_status(void (*say)(const char *fmt, ...));
#endif
