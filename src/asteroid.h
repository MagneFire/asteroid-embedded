#ifndef ASTEROID_H
#define ASTEROID_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "soc.h"
#include "board/board.h"

#define BIT(n) (1u << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static inline uint32_t readl(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline void writel(uint32_t v, uint32_t a) { *(volatile uint32_t *)(uintptr_t)a = v; }
static inline void dsb(void) { __asm__ volatile("dsb" ::: "memory"); }
static inline void dmb(void) { __asm__ volatile("dmb" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }
static inline void wfi(void) { __asm__ volatile("wfi" ::: "memory"); }
/* Keeps the compiler from moving memory accesses across it. Interrupts
 * all run on this one core, so this is enough to order a ring buffer's
 * data against the index that hands it between a handler and the main
 * loop: write the data, then the index; read the index, then the data,
 * then give the slot back. */
static inline void barrier(void) { __asm__ volatile("" ::: "memory"); }

extern char _start[];

/* lib */
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
unsigned long strtoul(const char *s, char **end, int base);

/* soc */
void mmu_init(void);
/* Adds a range to the map, in whole megabytes; soc_memory_map lists the
 * SoC's ranges with it. */
void mmu_map(uint32_t from, uint32_t to, uint32_t attr);
void mmu_map_to(uint32_t from, uint32_t to, uint32_t phys, uint32_t attr);
/* How far above the firmware's addresses DRAM really is. Whatever hands
 * a buffer to hardware gives it dma_addr. */
extern uint32_t phys_offset;
static inline uint32_t dma_addr(const volatile void *p) { return (uint32_t)(uintptr_t)p + phys_offset; }
static inline void *dma_ptr(uint32_t a) { return (void *)(uintptr_t)(a - phys_offset); }
void soc_memory_map(void);
#define MMU_RAM		0x1c0eu	/* cached */
#define MMU_RAM_UNCACHED	0x1c02u
#define MMU_DEVICE	0x0c16u
void cpu_clock_init(void);
void cpu_clock_log(void);
void cpu_clock_slow(bool slow);
/* Awake with nothing to draw: a SoC that can run slower meanwhile does. */
void cpu_clock_idle(bool idle);
bool cpu_clock_is_slow(void);
void cpu_clock_park(void);
void cpu_clock_unpark(void);
void power_request(bool sleep);
/* A wake request, with the cause logged if it wakes the watch. The first
 * request of a pass gives the cause. */
void power_wake(const char *cause);
/* Takes back a power_wake if this was its cause. */
void power_wake_cancel(const char *cause);
/* Asleep with the screen dark, shows the always-on face for a while. */
void power_peek(const char *cause);
enum tilt_mode { TILT_OFF, TILT_WAKE, TILT_FACE };
/* The daily step goal is set in units of STEP_GOAL_UNIT steps. */
#define STEP_GOAL_UNIT	500
#define STEP_GOAL_MAX	40
enum setting {
	SETTING_BRIGHTNESS,
	SETTING_BLUETOOTH,
	SETTING_VIBRATION,
	SETTING_AOD,
	SETTING_CLOCK12,
	SETTING_WATCHFACE,
	SETTING_NIGHTSTAND,
	SETTING_LAUNCHER,
	SETTING_TILT,
	SETTING_TAP,
	SETTING_FAHRENHEIT,
	SETTING_NIGHTSTAND_BRIGHTNESS,
	SETTING_NIGHTSTAND_DELAY,
	SETTING_NIGHTSTAND_AOD,
	SETTING_BURN_IN,
	SETTING_CINEMA_AOD,
	SETTING_NIGHTSTAND_ROTATE,
	SETTING_AUTO_BRIGHTNESS,
	SETTING_STEP_GOAL,
	SETTING_HEIGHT,
	SETTING_WEIGHT,
	SETTING_HR_INTERVAL,
	SETTING_AGE,
	SETTING_MALE,
	SETTINGS
};
int setting_get(enum setting s);
void setting_set(enum setting s, int v);
void settings_flush(void);	/* write changed settings to the store now */
void settings_poll(uint32_t now);	/* write them once they have been quiet for a while */
const char *setting_name(enum setting s);
void settings_changed(void);

/* The SoC's sleep states: set up, then each idle pass waits for an
 * interrupt in the deepest state the caller allows. A wake pin is a
 * line that has to end a deep state. */
bool cpu_idle_init(void);
void cpu_idle(bool cluster);
void wake_pin(unsigned pin, bool on, bool high);
void cpu_idle_stats(uint32_t *collapses, uint32_t *cluster_collapses, uint32_t *bailouts);
void dcache_clean_range(const void *addr, uint32_t size);
void dcache_invalidate_range(const void *addr, uint32_t size);
void dcache_clean_invalidate_range(const void *addr, uint32_t size);
void dcache_clean_invalidate_all(void);
void irq_ctrl_init(void);
void irq_attach(unsigned id, void (*fn)(void));
void irq_set_edge(unsigned id);
void irq_detach(unsigned id);
void irq_mask(unsigned id);
void irq_unmask(unsigned id);
#define GPIO_INPUT 0
#define GPIO_OUTPUT 1
#define GPIO_PULL_NONE 0
#define GPIO_PULL_DOWN 1
#define GPIO_PULL_UP 3
void gpio_config(unsigned n, unsigned func, unsigned dir, unsigned pull, unsigned drive_ma);
unsigned gpio_get(unsigned n);
void gpio_set(unsigned n, unsigned v);
/* The pin's configuration as the SoC keeps it, for the console. */
uint32_t gpio_raw_config(unsigned n);
int i2c_init(unsigned bus, unsigned sda, unsigned scl, unsigned func, uint32_t bus_hz);
int i2c_xfer(unsigned bus, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn);
typedef void (*i2c_done_fn)(int err, void *ctx);
int i2c_start(unsigned bus, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn, i2c_done_fn done, void *ctx);
bool i2c_busy(unsigned bus);
/* How long a slave may hold the clock on a transfer this master has on
 * the bus before it is given up, beyond the usual 20 ms; 0 for none. */
void i2c_set_timeout(unsigned bus, uint32_t stretch_ms);
bool haptics_init(void);
void haptics_play(const uint16_t *on_off_ms, unsigned n);
void haptics_poll(void);
bool haptics_busy(void);
void i2c_poll(unsigned bus);
/* SPI master on a QUP block, mode 0-3 (bit 1 CPOL, bit 0 CPHA), the clock the
 * nearest rate not above hz; pins are mosi, miso, clk. Chip select is the
 * caller's pin. spi_xfer clocks n bytes full duplex: tx NULL sends 0xff, rx
 * NULL drops the input. */
int spi_init(unsigned qup, const uint8_t pins[3], unsigned func, unsigned mode, uint32_t hz);
int spi_xfer(unsigned qup, const uint8_t *tx, uint8_t *rx, unsigned n);
int uart_init(unsigned uart, unsigned first_gpio, unsigned func, uint32_t baud, bool flow);
int uart_set_baud(unsigned uart, uint32_t baud);
int uart_write(unsigned uart, const uint8_t *p, unsigned n);
unsigned uart_read(unsigned uart, uint8_t *p, unsigned n);
bool uart_cts(unsigned uart);
void uart_stop(unsigned uart);
bool uart_idle(unsigned uart);
void uart_hold_rx(unsigned uart, bool hold);
void uart_stats(unsigned uart, uint32_t *rx, uint32_t *tx, uint32_t *errors);
#define GPIO_IRQ_LEVEL_LOW 0
#define GPIO_IRQ_LEVEL_HIGH 1
#define GPIO_IRQ_EDGE_FALLING 2
#define GPIO_IRQ_EDGE_RISING 3
#define GPIO_IRQ_EDGE_BOTH 4
void gpio_irq_init(void);
void gpio_irq_config(unsigned n, unsigned type, void (*fn)(unsigned n));
void gpio_irq_enable(unsigned n, bool on);
void gpio_irq_ack(unsigned n);
void irq_enable(void);
void irq_disable(void);
uint32_t irq_save(void);	/* mask IRQs, returning the state for irq_restore */
void irq_restore(uint32_t cpsr);
bool in_irq(void);	/* true inside an interrupt handler */
void timer_init(void);
void timer_resume(void);
uint32_t time_ms(void);
uint32_t time_us(void);
uint64_t timer_count(void);
uint32_t timer_hz(void);
uint32_t timer_ticks(void);
void timer_set_period(uint32_t ms);
void timer_next(uint32_t ms);
void delay_us(uint32_t us);
/* What is left of `ms` from `since`, at least 1: a deadline due now
 * still needs a pass. */
static inline uint32_t ms_left(uint32_t since, uint32_t ms, uint32_t now)
{
	uint32_t gone = now - since;
	return gone >= ms ? 1 : ms - gone;
}
void delay_ms(uint32_t ms);
void wdt_arm(uint32_t bark_ms, uint32_t bite_ms, void (*bark)(void));
void wdt_stop(void);
void wdt_feed(void);
uint8_t pmic_read(uint32_t addr);
void pmic_write(uint32_t addr, uint8_t val);
void power_off_arm(void);
const char *boot_reason(void);
bool power_key(void);
void power_key_irq_init(void);
bool power_key_irq(void);
uint32_t rtc_read(void);
void rtc_write(uint32_t secs);
/* False while a clock that does not keep the time itself has not been
 * set since the start. */
bool rtc_valid(void);
bool vbus_present(void);
void vbus_irq_init(void);
bool vbus_irq(void);
/* The watch's battery driver (board->battery_driver). */
void batt_init(void);
void batt_poll(void);
uint32_t batt_next_ms(void);
uint32_t batt_percent(void);
bool batt_charging(void);
bool batt_cable(void);
bool batt_usb(void);
void batt_cable_irq_init(void);
bool batt_cable_irq(void);
uint32_t batt_mv(void);
int32_t batt_ua(void);
int32_t batt_avg_ua(void);
const char *batt_status(void);
bool gauge_model_load(void);
/* The watch's light sensor (board->light): light_finish gives the lux
 * since light_start, or -1 without a reading. */
void light_init(void);
void light_start(void);
/* Holds the sensor powered between readings, for reading it often. */
void light_keep(bool on);
int32_t light_finish(void);
/* A supply by the number the PMIC gives it. */
int regulator_on(uint32_t id, uint32_t uv);
int regulator_off(uint32_t id);
/* What main does for the SoC: soc_init before anything else (clocks,
 * MMU, interrupts, timer, watchdog), soc_board_init once the log runs
 * and the watch is known (PMIC, supplies). */
void soc_init(void);
void soc_board_init(void);
void reboot(void) __attribute__((noreturn));
void reboot_fatal(const char *what) __attribute__((noreturn));
void fault_record(const char *kind, uint32_t pc, uint32_t lr, uint32_t sp, const uint32_t *regs);
extern bool reboot_to_bootloader;

/* log (persistent RAM, survives a warm reset) */
void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_puts(const char *s);
void log_record(const char *text);
const char *log_previous_record(void);
bool log_ring_lost(void);
/* Boot and charger history in the kv store (events.c). */
void events_poll(uint32_t now);
void events_append(char *buf, unsigned cap, const char *line);
void log_set_sink(void (*fn)(const char *s, size_t n));
void log_grep(const char *pat, unsigned lines, void (*out)(const char *s, size_t n, void *ctx), void *ctx);
uint32_t log_end(void);
uint32_t log_lines_from(unsigned lines);
size_t log_read(uint32_t *pos, uint32_t end, char *buf, size_t max);

/* display (see display/display.h) */
void fbcon_puts(const char *s);
void fbcon_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* usb */
void usb_init(const char *serialno);
void usb_cable(bool present, bool edge);
uint32_t usb_next_ms(void);
bool dcd_stuck(void);
void usb_pass(void);
void usb_poll(void);
bool usb_connected(void);
bool usb_busy(void);
void usb_stats(uint32_t *rx, uint32_t *tx);

struct civil { uint32_t year, month, day, weekday, hour, minute, second; };
void civil_from_epoch(uint32_t secs, struct civil *c);
uint32_t epoch_from_civil(const struct civil *c);

/* vendor firmware from the system partition */
const void *firmware_get(const char *name, uint32_t *size);
/* Part of a file of that partition by its path: the bytes read, or
 * negative. */
int firmware_read(const char *path, uint32_t off, void *buf, uint32_t len);

extern const char build_id[];
#endif
