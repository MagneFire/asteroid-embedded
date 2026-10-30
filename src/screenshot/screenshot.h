#ifndef SCREENSHOT_H
#define SCREENSHOT_H
#include <stdbool.h>

#define SCREENSHOT_BASE 0x0b000000u
#define SCREENSHOT_SIZE 0x00200000u

void screenshot_poll(void);
void screenshot_to_file(bool layer);
bool screenshot_busy(void);
#endif
