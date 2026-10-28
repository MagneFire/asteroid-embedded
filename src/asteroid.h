#ifndef ASTEROID_H
#define ASTEROID_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "soc/msm8226.h"

#define BIT(n) (1u << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static inline uint32_t readl(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline void writel(uint32_t v, uint32_t a) { *(volatile uint32_t *)(uintptr_t)a = v; }
static inline void dsb(void) { __asm__ volatile("dsb" ::: "memory"); }
static inline void dmb(void) { __asm__ volatile("dmb" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }
static inline void wfi(void) { __asm__ volatile("wfi" ::: "memory"); }

extern char _start[];

/* lib */
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
unsigned long strtoul(const char *s, char **end, int base);

/* soc */
void mmu_init(void);
void cpu_clock_init(void);
void cpu_clock_slow(bool slow);
bool cpu_clock_is_slow(void);
void power_request(bool sleep);

enum spm_mode { SPM_CLOCK_GATING, SPM_POWER_COLLAPSE };
bool spm_init(void);
void spm_cpu_mode(enum spm_mode mode);
void spm_idle(bool cluster);
void spm_stats(uint32_t *collapses, uint32_t *cluster_collapses, uint32_t *bailouts);
void dcache_clean_range(const void *addr, uint32_t size);
void dcache_invalidate_range(const void *addr, uint32_t size);
void dcache_clean_invalidate_range(const void *addr, uint32_t size);
void dcache_clean_invalidate_all(void);
void gic_init(void);
void gic_enable(unsigned id, void (*fn)(void));
void gic_set_edge(unsigned id);
uint32_t gic_cpu_save(void);
void gic_cpu_restore(uint32_t ppis);
void gic_disable(unsigned id);
void gic_mask(unsigned id);
void gic_unmask(unsigned id);
#define TLMM_INPUT 0
#define TLMM_OUTPUT 1
#define TLMM_PULL_NONE 0
#define TLMM_PULL_DOWN 1
#define TLMM_PULL_UP 3
void tlmm_config(unsigned n, unsigned func, unsigned dir, unsigned pull, unsigned drive_ma);
unsigned tlmm_get(unsigned n);
void tlmm_set(unsigned n, unsigned v);
int i2c_qup_init(unsigned qup, unsigned sda, unsigned scl, unsigned func, uint32_t bus_hz);
int i2c_qup_xfer(unsigned qup, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn);
typedef void (*i2c_done_fn)(int err, void *ctx);
int i2c_qup_start(unsigned qup, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn, i2c_done_fn done, void *ctx);
bool i2c_qup_busy(unsigned qup);
bool haptics_init(void);
void haptics_play(const uint16_t *on_off_ms, unsigned n);
void haptics_poll(void);
bool haptics_busy(void);
void i2c_qup_poll(unsigned qup);
int uart_dm_init(unsigned uart, unsigned first_gpio, unsigned func, uint32_t baud);
int uart_dm_set_baud(uint32_t baud);
int uart_dm_write(const uint8_t *p, unsigned n);
unsigned uart_dm_read(uint8_t *p, unsigned n);
bool uart_dm_cts(void);
void uart_dm_stop(void);
bool uart_dm_idle(void);
void uart_dm_stats(uint32_t *rx, uint32_t *tx, uint32_t *errors);
#define TLMM_IRQ_LEVEL_LOW 0
#define TLMM_IRQ_LEVEL_HIGH 1
#define TLMM_IRQ_EDGE_FALLING 2
#define TLMM_IRQ_EDGE_RISING 3
void tlmm_irq_init(void);
void tlmm_irq_config(unsigned n, unsigned type, void (*fn)(unsigned n));
void tlmm_irq_enable(unsigned n, bool on);
void tlmm_irq_ack(unsigned n);
void irq_enable(void);
void irq_disable(void);
void timer_init(void);
void timer_resume(void);
uint32_t time_ms(void);
uint32_t time_us(void);
uint64_t timer_count(void);
uint32_t timer_hz(void);
uint32_t timer_ticks(void);
void timer_set_period(uint32_t ms);
void delay_us(uint32_t us);
void delay_ms(uint32_t ms);
void wdt_arm(uint32_t bark_ms, uint32_t bite_ms, void (*bark)(void));
void wdt_feed(void);
void wdt_stop(void);
void spmi_init(void);
uint32_t spmi_version(void);
int spmi_read(uint32_t addr, uint8_t *buf, unsigned n);
int spmi_write(uint32_t addr, const uint8_t *buf, unsigned n);
int spmi_apid(uint32_t sid, uint32_t periph);
void spmi_irq_enable(int apid, bool on);
uint32_t spmi_irq_status(int apid);
void spmi_irq_clear(int apid, uint32_t bits);
uint8_t pmic_read(uint32_t addr);
void pmic_write(uint32_t addr, uint8_t val);
void pmic_init(void);
const char *pmic_pon_reason(void);
bool pmic_power_key(void);
void pmic_power_key_irq_init(void);
bool pmic_power_key_irq(void);
uint32_t pmic_rtc_read(void);
void pmic_rtc_write(uint32_t secs);
uint32_t pmic_battery_mv(void);
const char *pmic_charger_status(void);
bool pmic_charging(void);
int32_t pmic_battery_ua(void);
int32_t pmic_battery_avg_ua(void);
uint32_t pmic_battery_percent(uint32_t mv);
void pmic_charger_configure(void);
#define RPM_LDOA 0x616f646cu
#define RPM_SMPA 0x61706d73u
#define RPM_VSA 0x00617376u
bool rpm_init(void);
int rpm_request(uint32_t type, uint32_t id, const uint32_t *kvp, unsigned nkvp);
int rpm_regulator_enable(uint32_t type, uint32_t id, bool on);
int rpm_clock_rate(uint32_t type, uint32_t id, uint32_t khz);
void rpm_sleep_votes(void);
void rpm_bus_votes(void);
void dload_clear(void);
int scm_call2(uint32_t svc, uint32_t cmd, uint32_t arg1, uint32_t arg2);
int scm_wdog_debug_disable(void);
void reboot(void) __attribute__((noreturn));

/* log (persistent RAM, survives a warm reset) */
void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_puts(const char *s);
void log_record(const char *text);
const char *log_previous_record(void);
void log_set_sink(void (*fn)(const char *s, size_t n));
void log_grep(const char *pat, unsigned lines, void (*out)(const char *s, size_t n, void *ctx), void *ctx);
void log_tail(unsigned lines, void (*out)(const char *s, size_t n, void *ctx), void *ctx);

/* display (see display/display.h) */
void fbcon_puts(const char *s);
void fbcon_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* usb */
void usb_init(const char *serialno);
void usb_poll(void);
bool usb_connected(void);
void usb_stats(uint32_t *rx, uint32_t *tx);

struct civil { uint32_t year, month, day, weekday, hour, minute, second; };
void civil_from_epoch(uint32_t secs, struct civil *c);
uint32_t epoch_from_civil(const struct civil *c);

/* boot handover */
void boot_init(void);
const void *boot_blob(const char *name, uint32_t *size);

extern const char build_id[];
#endif
