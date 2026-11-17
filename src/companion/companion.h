/* Companion mode: the Apollo3 next to beluga's application processor,
 * answering its framework messages as the stock firmware does. In the
 * normal mode the AP has the panel and the touch controller; in the
 * wristband and power off modes the watch here has them. Until the AP
 * says otherwise the watch runs on its own, but for a reset other than
 * a power-on in the normal mode: then the AP keeps them. */
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
/* Whether a long flash write may go now. */
bool companion_store_ok(void);
/* No transfer under way, so the core may sleep deeply. */
bool companion_resting(void);
void companion_status(void (*say)(const char *fmt, ...));
#endif
