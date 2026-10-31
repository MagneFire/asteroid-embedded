#ifndef SCREENSHOT_H
#define SCREENSHOT_H
#include <stdbool.h>
#include "soc/memmap.h"

void screenshot_poll(void);
void screenshot_to_file(int layer);
bool screenshot_busy(void);
#endif
