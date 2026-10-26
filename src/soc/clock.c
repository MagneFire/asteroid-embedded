/* CPU cluster clock: a mux (APCS RCG at 0xf9011050) selects the crystal,
 * GPLL0 or the A7 PLL. Its setting survives a warm reset, so it is set
 * to the A7 PLL, undivided, at every boot. While the watch sleeps the
 * CPU runs from the 19.2 MHz crystal. */
#include "asteroid.h"

#define APCS_A7_CMD_RCGR	0xf9011050u
#define APCS_A7_CFG_RCGR	0xf9011054u
#define RCG_UPDATE		BIT(0)
#define SRC_XO			(0u << 8)
#define SRC_A7_PLL		(5u << 8)
#define DIV_1			1u

static void cpu_clock_select(uint32_t cfg)
{
	if (readl(APCS_A7_CFG_RCGR) == cfg)
		return;
	writel(cfg, APCS_A7_CFG_RCGR);
	writel(readl(APCS_A7_CMD_RCGR) | RCG_UPDATE, APCS_A7_CMD_RCGR);
	for (int i = 0; i < 100000 && (readl(APCS_A7_CMD_RCGR) & RCG_UPDATE); i++)
		;
}

void cpu_clock_init(void)
{
	cpu_clock_select(SRC_A7_PLL | DIV_1);
}

void cpu_clock_slow(bool slow)
{
	cpu_clock_select((slow ? SRC_XO : SRC_A7_PLL) | DIV_1);
}
