/* Forced include for host builds: the firmware's inline MMIO read is
 * renamed out of the way so console.c reaches the stub readl. */
#define readl fw_readl
#include "asteroid.h"
#undef readl
uint32_t readl(uint32_t a);
