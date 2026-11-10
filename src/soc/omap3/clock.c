/* Clocks. The bootloader leaves the PLLs locked and the MPU at the
 * speed its voltage allows, and they stay as they are; this reads them
 * for the log and gates the modules' clocks. The slow CPU clock is the
 * same PLL divided further, which needs no relock. */
#include "asteroid.h"

#define MPU_M2_SLOW	4

static uint32_t m2_fast = 1;
static bool slow;

uint32_t sys_clk_hz(void)
{
	static const uint32_t osc[] = { 12000000, 13000000, 19200000, 26000000, 38400000, 16800000 };
	uint32_t sel = readl(PRM_CLKSEL) & 7, div = readl(PRM_CLKSRC_CTRL) >> 6 & 3;
	uint32_t hz = sel < ARRAY_SIZE(osc) ? osc[sel] : 26000000;
	return div == 2 ? hz / 2 : hz;
}

void clock_enable(uint32_t cm, uint32_t bits, bool on)
{
	/* The wake-up domain's 32 kHz counter and USB have no functional
	 * clock bit. */
	uint32_t fbits = bits & ~((cm == CM_WKUP ? WKUP_32KSYNC : 0) | (cm == CM_CORE ? CORE_HSOTGUSB : 0));
	uint32_t flags = irq_save();
	if (on) {
		writel(readl(cm + CM_FCLKEN) | fbits, cm + CM_FCLKEN);
		writel(readl(cm + CM_ICLKEN) | bits, cm + CM_ICLKEN);
	} else {
		writel(readl(cm + CM_ICLKEN) & ~bits, cm + CM_ICLKEN);
		writel(readl(cm + CM_FCLKEN) & ~fbits, cm + CM_FCLKEN);
	}
	irq_restore(flags);
	/* IDLEST reads 0 for a module that can be reached. */
	if (on)
		for (unsigned i = 0; i < 10000 && (readl(cm + CM_IDLEST) & bits); i++)
			;
}

static uint32_t mpu_hz(void)
{
	uint32_t sel = readl(CM_MPU + CM_CLKSEL1_PLL), m = sel >> 8 & 0x7ff, n = sel & 0x7f;
	uint32_t m2 = readl(CM_MPU + CM_CLKSEL2_PLL) & 0x1f;
	if ((readl(CM_MPU + CM_CLKEN_PLL_MPU) & 7) != 7)
		return 0;	/* bypassed */
	return (uint32_t)((uint64_t)sys_clk_hz() * m / (n + 1) / (m2 ? m2 : 1));
}

void cpu_clock_init(void)
{
	m2_fast = readl(CM_MPU + CM_CLKSEL2_PLL) & 0x1f;
	if (!m2_fast)
		m2_fast = 1;
}

void cpu_clock_log(void)
{
	uint32_t core = readl(CM_CCR + CM_CLKSEL1_PLL), actlr;
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(actlr));
	log_printf("clock: crystal %lu Hz, cpu %lu Hz, core pll %08lx, actlr %08lx, reset status %08lx\n", sys_clk_hz(),
		   mpu_hz(), core, actlr, readl(PRM_RSTST));
}

void cpu_clock_slow(bool on)
{
	if (on == slow)
		return;
	slow = on;
	uint32_t m2 = on ? m2_fast * MPU_M2_SLOW : m2_fast;
	if (m2 > 16)
		m2 = 16;
	writel((readl(CM_MPU + CM_CLKSEL2_PLL) & ~0x1fu) | m2, CM_MPU + CM_CLKSEL2_PLL);
}

bool cpu_clock_is_slow(void) { return slow; }

/* Nothing to hand over before a sleep: the PLL stops by itself. */
void cpu_clock_park(void) {}
void cpu_clock_unpark(void) {}

/* The 26 MHz clock of the panel's bridge and the USB PHY, on while
 * either wants it. */
void periph_clock(bool on)
{
	static unsigned users;
	unsigned pin = board->periph_clock_gpio;
	if (!pin || (!on && !users))
		return;
	users += on ? 1 : -1;
	gpio_set(pin, users != 0);
	gpio_config(pin, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
}
