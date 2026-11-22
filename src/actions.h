#ifndef ACTIONS_H
#define ACTIONS_H
#include <stdbool.h>
#include <stdint.h>

/* Does nothing while the AP has the panel. */
void action_bluetooth_start(void);
/* The MCU's radio is off while the AP has the panel, as its own
 * Bluetooth serves the phone then; taken back, it starts again if the
 * setting has it on. */
void action_bluetooth_lend(bool to_ap, uint32_t now);
void action_setting_toggled(int s);
void action_set_clock(int kind, int v);
void action_setting_value(int s, int v);
int action_reset_watch(void);
#endif
