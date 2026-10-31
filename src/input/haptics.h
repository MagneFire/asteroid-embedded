#ifndef HAPTICS_H
#define HAPTICS_H
#include <stdbool.h>

/* What a vibration motor driver gives the pattern player in haptics.c:
 * bring the chip up with the motor off, then switch the motor. */
bool haptics_motor_init(void);
void haptics_motor(bool on);
#endif
