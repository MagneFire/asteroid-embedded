/* What main does for this SoC: the core at 48 MHz with the flash cache
 * on, interrupts, time and the pads. Without an MMU or a data cache in
 * front of SRAM there is no memory map to set up and no cache to keep. */
#include "asteroid.h"
#include "companion/companion.h"

#define CACHECFG		(CACHECTRL_BASE + 0x00)
#define CACHE_ENABLE		BIT(0)
#define CACHE_1WAY_1024		(8u << 4)
#define CACHE_ICACHE		BIT(8)
#define CACHE_DCACHE		BIT(9)
#define CACHE_CLKGATE		BIT(10)
#define CACHE_DATA_CLKGATE	BIT(20)
#define FPCCR_ASPEN_LSPEN	(3u << 30)
#define MCUCTRL_SIMOBUCK2	(MCUCTRL_BASE + 0x354)
#define MCUCTRL_SIMOBUCK3	(MCUCTRL_BASE + 0x358)
#define MCUCTRL_SIMOBUCK4	(MCUCTRL_BASE + 0x35c)
#define CHIPREV_A1		0x12u
#define CHIPREV_B0		0x21u

void mmu_init(void) {}
void mmu_map(uint32_t from, uint32_t to, uint32_t attr) {}
void soc_memory_map(void) {}
void dcache_clean_range(const void *addr, uint32_t size) {}
void dcache_invalidate_range(const void *addr, uint32_t size) {}
void dcache_clean_invalidate_range(const void *addr, uint32_t size) {}
void dcache_clean_invalidate_all(void) {}

void cpu_clock_init(void)
{
	writel(CLKKEY, CLKGEN_CLKKEY);
	writel(0, CLKGEN_CCTRL);
	writel(0, CLKGEN_CLKKEY);
	writel(readl(CACHECFG) & ~(CACHE_ICACHE | CACHE_DCACHE), CACHECFG);
	writel(CACHE_CLKGATE | CACHE_DATA_CLKGATE | CACHE_1WAY_1024 | CACHE_ICACHE | CACHE_DCACHE, CACHECFG);
	writel(readl(CACHECFG) | CACHE_ENABLE, CACHECFG);
	writel(readl(FPU_FPCCR) | FPCCR_ASPEN_LSPEN, FPU_FPCCR);
}

static void field(uint32_t addr, unsigned shift, unsigned bits, uint32_t v)
{
	uint32_t mask = ((1u << bits) - 1) << shift;
	writel((readl(addr) & ~mask) | v << shift, addr);
}

/* Vendor's low power init: SIMO buck trims, then the BLE buck as supply,
 * which the SIMO buck needs to run at all. */
static void low_power_init(void)
{
	uint32_t rev = readl(MCUCTRL_CHIPREV) & 0xff;
	if (rev >= CHIPREV_B0) {
		field(MCUCTRL_SIMOBUCK2, 16, 4, 2);
		field(MCUCTRL_SIMOBUCK2, 20, 4, 3);
		field(MCUCTRL_SIMOBUCK3, 0, 4, 5);
		field(MCUCTRL_SIMOBUCK3, 4, 4, 2);
		field(MCUCTRL_SIMOBUCK3, 8, 4, 6);
		field(MCUCTRL_SIMOBUCK3, 12, 4, 1);
		field(MCUCTRL_SIMOBUCK3, 27, 4, 3);
		field(MCUCTRL_SIMOBUCK4, 0, 4, 3);
	}
	if (rev >= CHIPREV_A1)
		field(MCUCTRL_SIMOBUCK4, 24, 1, 0);
	if (readl(PWRCTRL_DEVPWRSTATUS) & DEVPWRSTATUS_BLEL)
		return;
	writel(readl(MCUCTRL_FEATUREENABLE) | FEATURE_BLEREQ, MCUCTRL_FEATUREENABLE);
	for (unsigned us = 0; (readl(MCUCTRL_FEATUREENABLE) & FEATURE_BLE) != FEATURE_BLE; us += 10) {
		if (us >= 10000)
			return;
		delay_us(10);
	}
	writel(readl(PWRCTRL_SUPPLYSRC) | SUPPLYSRC_BLEBUCKEN, PWRCTRL_SUPPLYSRC);
	writel(readl(PWRCTRL_MISC) | MISC_MEMVRLPBLE, PWRCTRL_MISC);
}

void cpu_clock_log(void)
{
	log_printf("clock: core %u Hz, chip %08lx rev %08lx, supply %08lx, power %08lx, boot loader %08lx\n",
		   CORE_HZ, readl(MCUCTRL_CHIPPN), readl(MCUCTRL_CHIPREV), readl(PWRCTRL_SUPPLYSTATUS),
		   readl(PWRCTRL_DEVPWRSTATUS), readl(MCUCTRL_BOOTLOADER));
}

/* One speed: the burst mode's 96 MHz is not used. */
static bool slow;
void cpu_clock_slow(bool on) { slow = on; }
void cpu_clock_idle(bool on) {}
bool cpu_clock_is_slow(void) { return slow; }
void cpu_clock_park(void) {}
void cpu_clock_unpark(void) {}

void soc_init(void)
{
	cpu_clock_init();
	irq_ctrl_init();
	timer_init();
	low_power_init();
	gpio_init();
	swdbox_init();
}

void soc_board_init(void)
{
	for (const struct board_mcu_pad *p = board->mcu_pads; p && p->func != 0xff; p++)
		pad_set(p);
	if (board_chips_off())
		iom_stop(board->chips_off.i2c.bus);
	log_printf("boot: %s\n", boot_reason());
	log_printf("rtc: %s\n", rtc_origin());
	companion_boot();
}

/* No PMIC of its own: the application processor's has the charger and
 * the supplies. */
uint8_t pmic_read(uint32_t addr) { return 0; }
void pmic_write(uint32_t addr, uint8_t val) {}
int regulator_on(uint32_t id, uint32_t uv) { return -1; }
int regulator_off(uint32_t id) { return -1; }
bool vbus_present(void) { return false; }
void vbus_irq_init(void) {}
bool vbus_irq(void) { return false; }
