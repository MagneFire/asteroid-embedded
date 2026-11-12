/* Forced include for host tests of code that drives registers: every
 * register access and barrier goes to the test. */
#define readl fw_readl
#define writel fw_writel
#define dsb fw_dsb
#define dmb fw_dmb
#include "asteroid.h"
#undef readl
#undef writel
#undef dsb
#undef dmb
uint32_t readl(uint32_t a);
void writel(uint32_t v, uint32_t a);
void dsb(void);
void dmb(void);
