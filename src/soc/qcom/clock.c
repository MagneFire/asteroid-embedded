/* CPU cluster clock: a mux (APCS RCG) selects the crystal,
 * GPLL0 or the A7 PLL. Its setting survives a warm reset, so it is set
 * to the A7 PLL, undivided, at every boot. While the watch sleeps the
 * CPU runs from the 19.2 MHz crystal. The A7 PLL runs from the crystal
 * too, which is off while the cluster is powered down: before that the
 * CPU moves to GPLL0 / 2 and the PLL is turned off, afterwards the PLL
 * goes through its enable sequence again. */
#include "asteroid.h"

#define RCG_UPDATE		BIT(0)
#define SRC_XO			(0u << 8)
#define SRC_GPLL0		(4u << 8)
#define SRC_A7_PLL		(5u << 8)
#define DIV_2			3u
#define DIV_1			1u
#define A7_PLL_MODE		(A7_PLL_BASE + 0x00)
#define A7_PLL_L_VAL		(A7_PLL_BASE + 0x04)
#define A7_PLL_STATUS		(A7_PLL_BASE + 0x1c)
#define PLL_OUTCTRL		BIT(0)
#define PLL_BYPASSNL		BIT(1)
#define PLL_RESET_N		BIT(2)
#define GCC_GPLL_ENA_VOTE	GCC(chip->gpll0_vote)
#define GCC_GPLL0_STATUS	GCC(chip->gpll0_status)
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

/* The A7 PLL undivided on the APQ8026. The APQ8009W stays on GPLL0 / 2,
 * 400 MHz, which works at the lowest CPU voltage, until the firmware sets
 * the voltage itself. */
static uint32_t fast_cfg(void)
{
	return chip == &chip_8009 ? SRC_GPLL0 | DIV_2 : SRC_A7_PLL | DIV_1;
}

/* What the previous owner left, for the log once it is up. */
static uint32_t boot_cfg, boot_mode, boot_l, boot_status;

void cpu_clock_init(void)
{
	boot_cfg = readl(APCS_A7_CFG_RCGR);
	boot_mode = readl(A7_PLL_MODE);
	boot_l = readl(A7_PLL_L_VAL);
	boot_status = readl(A7_PLL_STATUS);
	cpu_clock_select(fast_cfg());
}

void cpu_clock_log(void)
{
	log_printf("cpu: clock at boot cfg %08lx, a7 pll mode %08lx l %lu status %08lx; now cfg %08lx status %08lx\n", boot_cfg,
		   boot_mode, boot_l, boot_status, readl(APCS_A7_CFG_RCGR), readl(A7_PLL_STATUS));
}

static bool slow_now;

void cpu_clock_idle(bool idle) {}

void cpu_clock_slow(bool slow)
{
	slow_now = slow;
	cpu_clock_select(slow ? SRC_XO | DIV_1 : fast_cfg());
}

bool cpu_clock_is_slow(void)
{
	return slow_now;
}

/* Without power collapse (the APQ8009W for now) the cluster is not
 * parked, so the CPU stays on the crystal while it waits. */
void cpu_clock_park(void)
{
	if (fast_cfg() != (SRC_A7_PLL | DIV_1))
		return;
	writel(readl(GCC_GPLL_ENA_VOTE) | BIT(0), GCC_GPLL_ENA_VOTE);
	for (int i = 0; i < 100000 && !(readl(GCC_GPLL0_STATUS) & chip->gpll0_ready); i++)
		;
	cpu_clock_select(SRC_GPLL0 | DIV_2);
	writel(readl(A7_PLL_MODE) & ~(PLL_OUTCTRL | PLL_BYPASSNL | PLL_RESET_N), A7_PLL_MODE);
}

void cpu_clock_unpark(void)
{
	if (fast_cfg() != (SRC_A7_PLL | DIV_1))
		return;
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
