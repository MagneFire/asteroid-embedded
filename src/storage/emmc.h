#ifndef EMMC_H
#define EMMC_H
#include <stdint.h>
#include <stdbool.h>

#define EMMC_BLOCK		512u
#define EMMC_WRITE_PARTITION	"cache"

struct emmc_info {
	bool ready, sector_mode, high_speed;
	unsigned bus_width;
	uint32_t generation, clock_hz, sectors, ocr, rca, mci_version, caps;
	uint32_t cid[4], csd[4];
	uint8_t ext_csd[512];
};

struct emmc_part {
	char name[37];
	uint32_t first, count;
};

int emmc_init(bool fast);
const struct emmc_info *emmc_card(void);
int emmc_read(uint32_t lba, uint32_t n, void *buf);
int emmc_write(uint32_t lba, uint32_t n, const void *buf);

int emmc_part_scan(void);
unsigned emmc_part_count(void);
const struct emmc_part *emmc_part_get(unsigned i);
int emmc_part_find(const char *name, uint32_t *first, uint32_t *count);

struct emmc_range { uint32_t first, count; };
int emmc_guard_set(const struct emmc_range *r, unsigned n);
void emmc_guard_clear(void);

uint32_t crc32(uint32_t crc, const void *buf, uint32_t n);
#endif
