/* Apollo3 Blue register map used by the firmware. */
#ifndef SOC_H
#define SOC_H
#include "memmap.h"

#define UI_HEAP_START		UI_HEAP_BASE

#define RSTGEN_BASE		0x40000000u
#define CLKGEN_BASE		0x40004000u
#define RTC_BASE		0x40004200u
#define CTIMER_BASE		0x40008000u
#define GPIO_BASE		0x40010000u
#define UART_BASE(n)		(0x4001c000u + 0x1000u * (n))
#define MCUCTRL_BASE		0x40020000u
#define PWRCTRL_BASE		0x40021000u
#define WDT_BASE		0x40024000u
#define IOS_BASE		0x50000000u
#define IOM_BASE(n)		(0x50004000u + 0x1000u * (n))
#define BLEIF_BASE		0x5000c000u
#define ADC_BASE		0x50010000u
#define CACHECTRL_BASE		0x40018000u

#define RSTGEN_SWPOI		(RSTGEN_BASE + 0x04)
#define RSTGEN_STAT		(RSTGEN_BASE + 0x0ffff000)
#define CLKGEN_OCTRL		(CLKGEN_BASE + 0x0c)
#define CLKGEN_CLKKEY		(CLKGEN_BASE + 0x14)
#define CLKGEN_CCTRL		(CLKGEN_BASE + 0x18)
#define CLKGEN_STATUS		(CLKGEN_BASE + 0x1c)
#define CLKGEN_FREQCTRL		(CLKGEN_BASE + 0x34)
#define CLKKEY			0x47u
#define PWRCTRL_SUPPLYSRC	(PWRCTRL_BASE + 0x00)
#define PWRCTRL_SUPPLYSTATUS	(PWRCTRL_BASE + 0x04)
#define PWRCTRL_DEVPWREN	(PWRCTRL_BASE + 0x08)
#define PWRCTRL_MEMPWDINSLEEP	(PWRCTRL_BASE + 0x0c)
#define PWRCTRL_MEMPWREN	(PWRCTRL_BASE + 0x10)
#define PWRCTRL_DEVPWRSTATUS	(PWRCTRL_BASE + 0x18)
#define PWRCTRL_MISC		(PWRCTRL_BASE + 0x24)
#define MCUCTRL_CHIPPN		(MCUCTRL_BASE + 0x00)
#define MCUCTRL_CHIPREV		(MCUCTRL_BASE + 0x0c)
#define MCUCTRL_FEATUREENABLE	(MCUCTRL_BASE + 0x18)
#define  FEATURE_BLEREQ		BIT(0)
#define  FEATURE_BLE		(BIT(0) | BIT(1) | BIT(2))	/* requested, acknowledged, available */
#define SUPPLYSRC_BLEBUCKEN	BIT(0)
#define MISC_MEMVRLPBLE		BIT(6)
#define DEVPWRSTATUS_BLEL	BIT(8)
#define MCUCTRL_BOOTLOADER	(MCUCTRL_BASE + 0x1a0)
#define MCUCTRL_DEBUGGER	(MCUCTRL_BASE + 0x20)

/* Peripheral power, the bit in DEVPWREN and the domain that reports it. */
#define PWR_IOS			BIT(0)
#define PWR_IOM(n)		BIT(1 + (n))
#define PWR_UART(n)		BIT(7 + (n))
#define PWR_ADC			BIT(9)
#define PWR_BLE			BIT(13)

/* Cortex-M4 system registers. */
#define SCB_ICSR		0xe000ed04u
#define SCB_VTOR		0xe000ed08u
#define SCB_AIRCR		0xe000ed0cu
#define SCB_SCR			0xe000ed10u
#define SCB_SHCSR		0xe000ed24u
#define SCB_CFSR		0xe000ed28u
#define SCB_HFSR		0xe000ed2cu
#define SCB_MMFAR		0xe000ed34u
#define SCB_BFAR		0xe000ed38u
#define SCB_CPACR		0xe000ed88u
#define FPU_FPCCR		0xe000ef34u
#define NVIC_ISER(n)		(0xe000e100u + 4u * (n))
#define NVIC_ICER(n)		(0xe000e180u + 4u * (n))
#define NVIC_ISPR(n)		(0xe000e200u + 4u * (n))
#define NVIC_ICPR(n)		(0xe000e280u + 4u * (n))
#define NVIC_IPR(n)		(0xe000e400u + (n))
#define DEMCR			0xe000edfcu
#define DWT_CTRL		0xe0001000u
#define DWT_CYCCNT		0xe0001004u

#define IRQ_COUNT		32
#define IRQ_WDT			1
#define IRQ_RTC			2
#define IRQ_IOS			4
#define IRQ_IOM(n)		(6 + (n))
#define IRQ_BLE			12
#define IRQ_GPIO		13
#define IRQ_CTIMER		14
#define IRQ_UART(n)		(15 + (n))
#define IRQ_ADC			18
#define IRQ_STIMER		22
#define IRQ_STIMER_CMPR(n)	(23 + (n))
#define IRQ_CLKGEN		31

#define CORE_HZ			48000000u
#define SLEEP_CLK_HZ		32768u
#define GPIO_COUNT		50
#define PMIC_ADDR_MAX		0u
/* The watchdog counts 8 bits at 16 Hz. */
#define WDT_MAX_MS		(255u * 1000u / 16u)

#define RAMOOPS_CONSOLE_BASE	RAMOOPS_BASE
#define RAMOOPS_CONSOLE_SIZE	0x10000u
#define RAMOOPS_RECORD_BASE	(RAMOOPS_BASE + RAMOOPS_CONSOLE_SIZE)
#define RAMOOPS_RECORD_SIZE	0x400u
#define RAMOOPS_RECORD_MAX	RAMOOPS_RECORD_SIZE
#define RAMOOPS_STAGE_BASE	(RAMOOPS_RECORD_BASE + RAMOOPS_RECORD_SIZE)

/* No eMMC, and less room for the profiler's samples. */
#define SOC_NO_EMMC		1
#define PROF_SAMPLES		1024

/* A link to the application processor (src/companion). */
#define SOC_AP_LINK		1

/* 96 MHz around draws and the flash cache's counters (burst.c). */
#define SOC_BURST		1

/* Reset watch in the quick settings and on the console. */
#define SOC_RESET_WATCH		1

/* Smaller records than the eMMC store's: a slot is one flash page. */
#define KV_SLOT_SIZE		0x2000u

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stdint.h>

/* A pad: function select 0-7 (3 is GPIO on every pad), PAD_* flags. */
#define PAD_INPUT		BIT(0)
#define PAD_PULL_UP		BIT(1)
#define PAD_STRONG		BIT(2)	/* 12 mA instead of 2 */
#define PAD_OPEN_DRAIN		BIT(3)
/* An I2C pad's pull-up of 1.5 kOhm, which the bus needs. */
#define PAD_I2C_PULL		BIT(4)
/* A chip select pad: which IOM's select it carries is its output
 * mode, 0-3, from the datasheet's table. */
#define PAD_NCE(outcfg)		(BIT(5) | (outcfg) << 6)
/* A GPIO driven from boot on, high or low. */
#define PAD_OUT_HIGH		BIT(8)
#define PAD_OUT_LOW		BIT(9)
#define PAD_FUNC_GPIO		3
#define PAD_SWDCK		20
#define PAD_SWDIO		21
void pad_config(unsigned pad, unsigned func, unsigned flags);
struct board_mcu_pad;
void pad_set(const struct board_mcu_pad *p);
void periph_power(uint32_t bits, bool on);
/* The flash's read wait states (init.c). */
void flash_wait(uint32_t n);
uint32_t flash_wait_now(void);
/* The core's clock: CORE_HZ, twice that in a burst (burst.c). */
extern uint32_t cpu_hz;
/* Spins for at least that many core cycles. */
void delay_cycles(uint32_t n);
/* Brings the next tick forward to within ms, never later. */
void timer_wake_within(uint32_t ms);

/* SPI masters on the IOMs: the bus at its rate and SPI mode, then
 * transfers to a chip select of it, the bytes of each call one burst
 * with the select held through it; cont holds it after the call for
 * the next one. iom_spi_write_start hands a long write to DMA and returns:
 * iom_spi_busy says when it is done, and the buffer must stay until then. */
int iom_spi_init(unsigned bus, uint32_t hz, unsigned mode);
int iom_spi_write(unsigned bus, unsigned cs, const void *p, unsigned n, bool cont);
int iom_spi_read(unsigned bus, unsigned cs, void *p, unsigned n, bool cont);
/* A new rate for the transfers after, once those before are done. */
void iom_spi_set_hz(unsigned bus, uint32_t hz, unsigned mode);
int iom_spi_write_start(unsigned bus, unsigned cs, const void *p, unsigned n, bool cont);
bool iom_spi_busy(unsigned bus);
int iom_spi_wait(unsigned bus);
/* Powers an IOM down; iom_spi_init or i2c_init starts it again. */
void iom_stop(unsigned bus);

/* The IO slave (ios.c): fn gets the interrupt status, IOS_WRITTEN for a
 * host write that reached the host interrupt register at 0x78 (the AP's
 * 121st byte of each write part), IOS_READ for a host read that reached
 * the FIFO. ios_read copies from the direct area; ios_stage fills it and
 * the FIFO for the host's next read. */
#define IOS_WRITTEN		BIT(5)
#define IOS_READ		BIT(6)
void ios_init(void (*fn)(uint32_t status));
void ios_read(unsigned off, uint8_t *p, unsigned n);
void ios_stage(const uint8_t *direct, unsigned dn, const uint8_t *fifo, unsigned fn);
uint32_t ios_interrupts(void);

/* Internal flash through the boot ROM's helpers, inside the store's
 * pages only. */
int flash_erase_page(uint32_t addr);
int flash_program(uint32_t addr, const uint32_t *words, uint32_t n);

/* The battery's voltage from the ADC, in mV, or 0 without a reading. */
uint32_t adc_battery_mv(void);
/* The charge the application processor's gauge reports. */
void battery_from_ap(unsigned percent);
/* RSTGEN's status bits at the start. */
uint32_t reset_status(void);
/* Where the wall clock came from: kept across a reset, set, or not set. */
const char *rtc_origin(void);

void gpio_init(void);
void gpio_swdck_irq(void (*fn)(unsigned n));
/* The pad's edge latched, whether or not its interrupt is on. */
bool gpio_irq_latched(unsigned n);
void wdt_irq_init(void);
/* The console over SWD (swdbox.c): input the application processor
 * left, and output for it to collect. */
void swdbox_init(void);
void swdbox_poll(void);
/* The SWD clock wakes the core (swdwake.c), which then stays out of
 * deep sleep while the application processor talks and while debug is
 * on. */
void swdwake_init(void);
bool swd_wanted(void);
/* TurboSPOT (burst.c): the core at 96 MHz from burst_draw_begin, which
 * says whether it got it, to burst_draw_end or burst_leave; the cache's
 * counters around a draw when the console asks. */
void burst_init(void);
void burst_log(void);
bool burst_draw_begin(void);
void burst_draw_end(bool drawn, bool fast, uint32_t us);
void burst_leave(void);
void burst_command(const char *args, void (*say)(const char *fmt, ...));
void cache_command(const char *args, void (*say)(const char *fmt, ...));
#endif
#endif
