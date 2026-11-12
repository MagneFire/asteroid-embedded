/* Forced include for host builds: the firmware's inline MMIO read and
 * barriers are renamed out of the way so console.c reaches the stub
 * readl and the barriers build for the host. */
#define readl fw_readl
#define dsb fw_dsb
#define dmb fw_dmb
#include "asteroid.h"
#undef readl
#undef dsb
#undef dmb
uint32_t readl(uint32_t a);
static inline void dsb(void) { __sync_synchronize(); }
static inline void dmb(void) { __sync_synchronize(); }
