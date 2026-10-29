/* CPU cluster clock: a mux (APCS RCG at 0xf9011050) selects the crystal,
 * GPLL0 or the A7 PLL. Its setting survives a warm reset, so it is set
 * to the A7 PLL, undivided, at every boot. While the watch sleeps the
 * CPU runs from the 19.2 MHz crystal. The A7 PLL runs from the crystal
 * too, which is off while the cluster is powered down: before that the
 * CPU moves to GPLL0 / 2 and the PLL is turned off, afterwards the PLL
 * goes through its enable sequence again. */
#include "asteroid.h"

#define APCS_A7_CMD_RCGR	0xf9011050u
#define APCS_A7_CFG_RCGR	0xf9011054u
#define RCG_UPDATE		BIT(0)
#define SRC_XO			(0u << 8)
#define SRC_GPLL0		(4u << 8)
#define SRC_A7_PLL		(5u << 8)
#define DIV_2			3u
#define DIV_1			1u
#define A7_PLL_MODE		0xf9016000u
#define A7_PLL_STATUS		0xf901601cu
#define PLL_OUTCTRL		BIT(0)
#define PLL_BYPASSNL		BIT(1)
#define PLL_RESET_N		BIT(2)
#define GCC_GPLL_ENA_VOTE	0xfc401480u
#define GCC_GPLL0_STATUS	0xfc40001cu
#define A7_PLL_LOCKED		BIT(16)

static void cpu_clock_select(uint32_t cfg)
{
	if (readl(APCS_A7_CFG_RCGR) == cfg)
		return;
	if ((cfg & SRC_A7_PLL) == SRC_A7_PLL)
		for (int i = 0; i < 100000 && !(readl(A7_PLL_STATUS) & A7_PLL_LOCKED); i++)
			;
	writel(cfg, APCS_A7_CFG_RCGR);
	writel(readl(APCS_A7_CMD_RCGR) | RCG_UPDATE, APCS_A7_CMD_RCGR);
	for (int i = 0; i < 100000 && (readl(APCS_A7_CMD_RCGR) & RCG_UPDATE); i++)
		;
}

void cpu_clock_init(void)
{
	cpu_clock_select(SRC_A7_PLL | DIV_1);
}

static bool slow_now;

void cpu_clock_slow(bool slow)
{
	slow_now = slow;
	cpu_clock_select((slow ? SRC_XO : SRC_A7_PLL) | DIV_1);
}

bool cpu_clock_is_slow(void)
{
	return slow_now;
}

void cpu_clock_park(void)
{
	writel(readl(GCC_GPLL_ENA_VOTE) | BIT(0), GCC_GPLL_ENA_VOTE);
	for (int i = 0; i < 100000 && !(readl(GCC_GPLL0_STATUS) & BIT(17)); i++)
		;
	cpu_clock_select(SRC_GPLL0 | DIV_2);
	writel(readl(A7_PLL_MODE) & ~(PLL_OUTCTRL | PLL_BYPASSNL | PLL_RESET_N), A7_PLL_MODE);
}

void cpu_clock_unpark(void)
{
	uint32_t mode = readl(A7_PLL_MODE) | PLL_BYPASSNL;
	writel(mode, A7_PLL_MODE);
	dsb();
	delay_us(10);
	mode |= PLL_RESET_N;
	writel(mode, A7_PLL_MODE);
	for (int i = 0; i < 200 && !(readl(A7_PLL_STATUS) & A7_PLL_LOCKED); i++)
		delay_us(1);
	writel(mode | PLL_OUTCTRL, A7_PLL_MODE);
	dsb();
}
