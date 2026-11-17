/* The Apollo3 seen from the application processor over SWD: memory
 * reads and writes are all the link gives, and everything else (halting
 * the core, its registers, calling the boot ROM's flash helpers) is done
 * through the core's debug registers with them. */
#ifndef SWD_H
#define SWD_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* read_block and write_block move n bytes of whole words at once, 0 on
 * success; either may be NULL, and a failed one is done again word by
 * word. */
struct swd_link {
	int (*read)(void *ctx, uint32_t addr, uint32_t *val);
	int (*write)(void *ctx, uint32_t addr, uint32_t val);
	void (*sleep_us)(void *ctx, uint32_t us);
	void *ctx;
	int (*read_block)(void *ctx, uint32_t addr, void *buf, uint32_t n);
	int (*write_block)(void *ctx, uint32_t addr, const void *buf, uint32_t n);
};

#define SWD_FLASH_SIZE		0x100000u
#define SWD_FLASH_PAGE		0x2000u
/* Nothing below this is ever erased or written: the boot loader. */
#define SWD_FLASH_FIRST		0x0000c000u
#define SWD_SRAM_BASE		0x10000000u
#define SWD_SRAM_END		0x10060000u
#define SWD_INFO0_BASE		0x50020000u
#define SWD_INFO0_SIZE		0x2000u
#define SWD_BOX_BASE		0x1005f800u
#define SWD_ROM_BASE		0x08000000u
#define SWD_ROM_END		0x08010000u

int swd_read(const struct swd_link *l, uint32_t addr, uint32_t *val);
int swd_write(const struct swd_link *l, uint32_t addr, uint32_t val);
int swd_read_block(const struct swd_link *l, uint32_t addr, uint32_t *buf, uint32_t words);
int swd_write_block(const struct swd_link *l, uint32_t addr, const uint32_t *buf, uint32_t words);

int swd_halt(const struct swd_link *l);
int swd_run(const struct swd_link *l);
bool swd_halted(const struct swd_link *l);
int swd_reg_read(const struct swd_link *l, unsigned reg, uint32_t *val);
int swd_reg_write(const struct swd_link *l, unsigned reg, uint32_t val);

/* Halts the core with the watchdog and the interrupts off, and faults
 * caught as halts, for loading or flashing: -3 if the watchdog is
 * locked on, -4 if the core keeps being halted inside the boot ROM. */
int swd_take(const struct swd_link *l);
/* Leaves the core in a loop in SRAM with the interrupts off, for when it
 * must not run from flash and the debug session ends. */
int swd_park(const struct swd_link *l);
/* A system reset requested from a halt, after which the chip runs from
 * its boot loader. */
int swd_reset(const struct swd_link *l);

/* Why an image is refused, or NULL: its vector table must point into
 * SRAM for the stack and into itself for the entry. */
const char *swd_image_check(const uint8_t *img, uint32_t size, uint32_t base);
/* Why loading size bytes at base in SRAM is refused, or NULL. */
const char *swd_ram_check(uint32_t base, uint32_t size);
/* Why a flash write of size bytes at addr is refused, or NULL. */
const char *swd_flash_check(uint32_t addr, uint32_t size);

/* Loads an image linked at an SRAM address and starts it there. */
int swd_ram_run(const struct swd_link *l, const uint8_t *img, uint32_t size, uint32_t base);
/* Erases and programs whole pages from addr and reads them back, the
 * first page last; progress returning false stops it (-7). Any failure
 * from -3 on can leave the first page erased. */
int swd_flash(const struct swd_link *l, uint32_t addr, const uint8_t *img, uint32_t size,
	      bool (*progress)(uint32_t done, uint32_t total));

uint32_t swd_crc32(uint32_t crc, const void *buf, size_t n);
#endif
