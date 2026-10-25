/* Top-level mode mux: one configuration word per pin (pull, function,
 * drive strength, output enable) and an in/out word. */
#include "asteroid.h"

void tlmm_config(unsigned n, unsigned func, unsigned dir, unsigned pull, unsigned drive_ma)
{
	uint32_t v = (pull & 3) | (func & 0xf) << 2 | ((drive_ma / 2 - 1) & 7) << 6 | (dir ? BIT(9) : 0);
	writel(v, TLMM_GPIO_CFG(n));
}

unsigned tlmm_get(unsigned n)
{
	return readl(TLMM_GPIO_IO(n)) & 1;
}

void tlmm_set(unsigned n, unsigned v)
{
	writel(v ? BIT(1) : 0, TLMM_GPIO_IO(n));
}
