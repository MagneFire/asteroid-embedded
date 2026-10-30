#ifndef A3XX_H
#define A3XX_H
#include <stdint.h>
#include <stdbool.h>

bool gpu_init(void);
bool gpu_selftest(void);
void gpu_suspend(void);
bool gpu_ready(void);
#endif
