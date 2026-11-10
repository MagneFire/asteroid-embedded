/* OMAP3630 register map used by the firmware. */
#ifndef SOC_H
#define SOC_H
#include "memmap.h"

/* System control module: the pads and the scratchpad the bootloader
 * reads after a reset. */
#define CONTROL_BASE		0x48002000u
#define CONTROL_PADCONF(off)	(CONTROL_BASE + 0x30 + (off))	/* 16 bits per pad */
#define CONTROL_SCRATCHPAD_BOOT	(CONTROL_BASE + 0x914)
#define CONTROL_PADCONF_WKUP(off) (0x48002a00u + (off))
/* The bootloader left the panel showing its picture, set up as the
 * display driver would. */
bool boot_panel_lit(void);
#define CONTROL_PBIAS_LITE	(CONTROL_BASE + 0x520)
#define PBIAS_3V_0		BIT(0)
#define PBIAS_ON_0		BIT(1)
#define PBIAS_FAST_0		BIT(2)
#define PBIAS_3V_1		BIT(8)
#define PBIAS_ON_1		BIT(9)
#define CONTROL_WKUP_CTRL	(CONTROL_BASE + 0xa5c)
#define WKUP_SIM_IO_ON		BIT(6)

/* Clock and power: CM for the clocks, PRM for resets, power states and
 * wake-ups. Each module's registers sit at the same offsets. */
#define CM_BASE			0x48004000u
#define CM_MPU			(CM_BASE + 0x900)
#define CM_CORE			(CM_BASE + 0xa00)
#define CM_WKUP			(CM_BASE + 0xc00)
#define CM_CCR			(CM_BASE + 0xd00)
#define CM_DSS			(CM_BASE + 0xe00)
#define CM_PER			(CM_BASE + 0x1000)
#define CM_FCLKEN		0x00
#define CM_FCLKEN1		0x00
#define CM_ICLKEN		0x10
#define CM_ICLKEN1		0x10
#define CM_IDLEST		0x20
#define CM_AUTOIDLE		0x30
#define CM_CLKSEL		0x40
#define CM_CLKSEL1_PLL		0x40	/* in CM_MPU and CM_CCR */
#define CM_CLKSEL2_PLL		0x44
#define CM_CLKSTCTRL		0x48
#define CM_CLKEN_PLL		0x00	/* in CM_CCR */
#define CM_PWRDN_96M		BIT(27)
#define CM_CLKEN_PLL_MPU	0x04	/* in CM_MPU */
#define CM_IDLEST_PLL_MPU	0x24

#define PRM_BASE		0x48306000u
#define PRM_MPU			(PRM_BASE + 0x900)
#define PRM_CORE		(PRM_BASE + 0xa00)
#define PRM_WKUP		(PRM_BASE + 0xc00)
#define PRM_CCR			(PRM_BASE + 0xd00)
#define PRM_PER			(PRM_BASE + 0x1000)
#define PRM_GLBL		(PRM_BASE + 0x1200)
#define PRM_CLKSEL		(PRM_CCR + 0x40)	/* the crystal's frequency */
#define PRM_RSTCTRL		(PRM_GLBL + 0x50)
#define PRM_RSTST		(PRM_GLBL + 0x58)
#define PRM_CLKSRC_CTRL		(PRM_GLBL + 0x70)

/* Clock enable bits, the same in FCLKEN and ICLKEN. */
#define CORE_HSOTGUSB		BIT(4)	/* interface clock only */
#define CORE_UART1		BIT(13)
#define CORE_UART2		BIT(14)
#define CORE_I2C(n)		BIT(14 + (n))	/* 1 to 3 */
#define CORE_MCSPI1		BIT(18)
#define CORE_MMC2		BIT(25)
#define WKUP_GPT1		BIT(0)
#define WKUP_32KSYNC		BIT(2)	/* interface clock only */
#define WKUP_GPIO1		BIT(3)
#define WKUP_WDT2		BIT(5)
#define PER_GPT2		BIT(3)
#define PER_GPIO(n)		BIT(11 + (n))	/* 2 to 6 */

#define INTC_BASE		0x48200000u
#define IRQ_COUNT		96
#define IRQ_SYS_NIRQ		7	/* the PMIC */
#define IRQ_PRCM		11
#define IRQ_DSS			25
#define IRQ_GPIO(bank)		(28 + (bank))	/* 1 to 6 */
#define IRQ_GPT1		37
#define IRQ_GPT2		38
#define IRQ_I2C1		56
#define IRQ_I2C2		57
#define IRQ_I2C3		61
#define IRQ_MCSPI1		65
#define IRQ_UART1		72
#define IRQ_UART2		73
#define IRQ_MMC2		86
#define IRQ_MUSB		92
#define USB_IRQ			IRQ_MUSB

#define GPT1_BASE		0x48318000u
#define GPT2_BASE		0x49032000u
#define SYNC32K_CR		0x48320010u
#define SLEEP_CLK_HZ		32768u
#define WDT2_BASE		0x48314000u

#define GPIO_COUNT		192
#define I2C_BASES		{ 0x48070000u, 0x48072000u, 0x48060000u }
#define MCSPI1_BASE		0x48098000u
#define UART1_BASE		0x4806a000u
#define UART2_BASE		0x4806c000u
#define MMC2_BASE		0x480b4000u
#define MUSB_BASE		0x480ab000u
#define DSS_BASE		0x48050000u
#define DISPC_BASE		0x48050400u
#define DSI_BASE		0x4804fc00u

/* The log region as the vendor kernel's ramoops reads it with this
 * watch's tree: the console ring, then the 1 KiB the kernel calls the
 * annotation, which holds the fault record. The kernel rounds the
 * tree's 0x1fc00 for the console down to a power of two. */
#define RAMOOPS_CONSOLE_BASE	RAMOOPS_BASE
#define RAMOOPS_CONSOLE_SIZE	0x10000u
#define RAMOOPS_RECORD_BASE	(RAMOOPS_BASE + RAMOOPS_CONSOLE_SIZE)
#define RAMOOPS_RECORD_SIZE	0x400u

#define PMIC_ADDR_MAX		0xffu
/* The watchdog counts 32 bits of the 32 kHz clock. */
#define WDT_MAX_MS		(0xffffffffu / SLEEP_CLK_HZ * 1000u)

#ifndef __ASSEMBLER__
/* Clocks: the crystal, and the functional and interface clock of a
 * module on or off (cm is CM_CORE, CM_WKUP or CM_PER). */
uint32_t sys_clk_hz(void);
void clock_enable(uint32_t cm, uint32_t bits, bool on);

/* A pad by its offset in the pad configuration: mux mode 0 to 7 with
 * PAD_* flags. */
#define PAD_PULL_ENABLE		BIT(3)
#define PAD_PULL_UP		BIT(4)
#define PAD_INPUT		BIT(8)
#define PAD_WAKEUP		BIT(14)
#define PAD_MODE_GPIO		4
#define PAD_MODE_SAFE		7
void pad_config(uint32_t off, uint32_t value);
uint32_t pad_read(uint32_t off);

void gpio_init(void);
void periph_clock(bool on);
/* From the tick: the watchdog's bark is a check there. */
void wdt_tick(void);

/* TPS65912: its registers are pmic_read and pmic_write. */
bool tps65912_init(void);
/* A deep sleep stopped the clock that time is counted from for this many
 * 32 kHz ticks. */
void timer_slept(uint32_t ticks32k);
#endif
#endif
