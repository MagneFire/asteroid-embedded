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
void dcache_clean_range(const void *addr, uint32_t size);
void dcache_invalidate_range(const void *addr, uint32_t size);
void dcache_clean_invalidate_range(const void *addr, uint32_t size);
void dcache_clean_invalidate_all(void);
void gic_init(void);
void gic_enable(unsigned id, void (*fn)(void));
void gic_disable(unsigned id);
void irq_enable(void);
void irq_disable(void);
void timer_init(void);
uint32_t time_ms(void);
uint32_t time_us(void);
uint32_t timer_hz(void);
uint32_t timer_ticks(void);
void delay_us(uint32_t us);
void delay_ms(uint32_t ms);
void wdt_arm(uint32_t bark_ms, uint32_t bite_ms, void (*bark)(void));
void wdt_feed(void);
void wdt_stop(void);
void spmi_init(void);
uint32_t spmi_version(void);
int spmi_read(uint32_t addr, uint8_t *buf, unsigned n);
int spmi_write(uint32_t addr, const uint8_t *buf, unsigned n);
uint8_t pmic_read(uint32_t addr);
void pmic_write(uint32_t addr, uint8_t val);
void pmic_init(void);
const char *pmic_pon_reason(void);
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

/* display (see display/display.h) */
void fbcon_puts(const char *s);
void fbcon_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

extern const char build_id[];
#endif
