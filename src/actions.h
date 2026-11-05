#ifndef ACTIONS_H
#define ACTIONS_H

void action_bluetooth_start(void);
void action_setting_toggled(int s);
void action_set_clock(int kind, int v);
void action_setting_value(int s, int v);
#endif
