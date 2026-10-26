/* CPU cluster clock: a mux (APCS RCG at 0xf9011050) selects the crystal,
 * GPLL0 or the A7 PLL. Its setting survives a warm reset, so it is set
 * to the A7 PLL, undivided, at every boot. */
#include "asteroid.h"

#define APCS_A7_CMD_RCGR	0xf9011050u
#define APCS_A7_CFG_RCGR	0xf9011054u
#define RCG_UPDATE		BIT(0)
#define SRC_A7_PLL		(5u << 8)
#define DIV_1			1u

void cpu_clock_init(void)
{
	uint32_t cfg = readl(APCS_A7_CFG_RCGR);
	if (cfg == (SRC_A7_PLL | DIV_1))
		return;
	writel(SRC_A7_PLL | DIV_1, APCS_A7_CFG_RCGR);
	writel(readl(APCS_A7_CMD_RCGR) | RCG_UPDATE, APCS_A7_CMD_RCGR);
	for (int i = 0; i < 100000 && (readl(APCS_A7_CMD_RCGR) & RCG_UPDATE); i++)
		;
}
