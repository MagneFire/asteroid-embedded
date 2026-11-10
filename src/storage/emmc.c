/* eMMC: the card's side, on the SoC's host controller (emmc_host.h).
 * Every init starts from CMD0, as a bootloader may leave the card
 * deselected and asleep. Data moves through an aligned bounce buffer.
 * Writes are refused unless the whole range lies inside one range of
 * the write guard; the guard only accepts ranges inside the
 * EMMC_WRITE_PARTITION partition. */
#include <string.h>
#include "asteroid.h"
#include "emmc.h"
#include "emmc_host.h"

#define EMMC_IDLE_MS	3000

#define OCR_REQUEST		0x40ff8000u
#define OCR_READY		BIT(31)
#define OCR_SECTOR		BIT(30)
#define RCA			2u
#define STATUS_READY		BIT(8)
#define STATUS_STATE(s)		(((s) >> 9) & 0xf)
#define STATE_TRAN		4u
#define STATUS_ERRORS		0xfdf90000u
#define STATUS_SWITCH_ERROR	BIT(7)
#define SWITCH_WRITE_BYTE	3u
#define SWITCH_CLEAR_BITS	2u
#define EXT_CSD_BUS_WIDTH	183
#define EXT_CSD_HS_TIMING	185
#define EXT_CSD_PART_CONFIG	179
#define EXT_CSD_CARD_TYPE	196
#define EXT_CSD_SEC_COUNT	212

#define MAX_PARTS		64u
#define MAX_GUARD		32u

static struct emmc_info card;
static struct emmc_part parts[MAX_PARTS];
static unsigned nparts;
static struct emmc_range guard[MAX_GUARD];
static unsigned nguard;
static uint32_t guard_first, guard_count;

uint8_t emmc_bounce[EMMC_XFER_BLOCKS * EMMC_BLOCK] __attribute__((aligned(64)));
static uint8_t gpt_buf[32 * EMMC_BLOCK] __attribute__((aligned(64)));

uint32_t crc32(uint32_t crc, const void *buf, uint32_t n)
{
	const uint8_t *p = buf;
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
	}
	return ~crc;
}

static int cmd(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *resp)
{
	struct emmc_cmd c = { .index = index, .arg = arg, .flags = flags };
	int r = emmc_host_send(&c);
	if (!r && resp)
		*resp = c.resp[0];
	if (!r && (flags & CMD_INDEX) && (c.resp[0] & STATUS_ERRORS)) {
		log_printf("emmc: cmd%lu arg %08lx: status %08lx\n", index, arg, c.resp[0]);
		return -2;
	}
	return r;
}

static int card_status(uint32_t *st)
{
	return cmd(13, RCA << 16, R1, st);
}

static int wait_tran(uint32_t timeout_ms)
{
	uint32_t t0 = time_ms(), st = 0;
	for (;;) {
		int r = card_status(&st);
		if (r)
			return r;
		if ((st & STATUS_READY) && STATUS_STATE(st) == STATE_TRAN)
			return 0;
		if (time_ms() - t0 > timeout_ms) {
			log_printf("emmc: card stuck, status %08lx\n", st);
			return -3;
		}
		wdt_feed();
	}
}

static int ext_csd_switch(uint32_t access, uint32_t index, uint32_t value)
{
	int r = cmd(6, access << 24 | index << 16 | value << 8, R1B, NULL);
	if (r)
		return r;
	uint32_t st;
	if ((r = wait_tran(1000)))
		return r;
	if ((r = card_status(&st)))
		return r;
	return (st & STATUS_SWITCH_ERROR) ? -4 : 0;
}

static int read_ext_csd(void)
{
	struct emmc_cmd c = { .index = 8, .flags = R1, .blocks = 1, .blksz = 512, .read = true };
	int r = emmc_host_send(&c);
	if (!r)
		memcpy(card.ext_csd, emmc_bounce, 512);
	return r;
}

static int card_ready(uint32_t *ocr)
{
	int r;
	if ((r = cmd(0, 0, R_NONE, NULL)))
		return r;
	delay_ms(1);
	uint32_t t0 = time_ms();
	do {
		if ((r = cmd(1, OCR_REQUEST, R3, ocr)))
			return r;
		if (*ocr & OCR_READY) {
			log_printf("emmc: ocr %08lx after %lu ms\n", *ocr, time_ms() - t0);
			return 0;
		}
		delay_ms(1);
		wdt_feed();
	} while (time_ms() - t0 < 1500);
	log_printf("emmc: card never left busy, ocr %08lx\n", *ocr);
	return -5;
}

static int card_identify(void)
{
	struct emmc_cmd c;
	uint32_t ocr = 0;
	emmc_host_open_drain(true);
	int r = card_ready(&ocr);
	if (r) {
		log_printf("emmc: no answer to CMD1 (%d), waking the card from sleep\n", r);
		cmd(5, RCA << 16, R1B, NULL);
		if ((r = card_ready(&ocr)))
			return r;
	}
	card.ocr = ocr;
	card.sector_mode = ocr & OCR_SECTOR;
	c = (struct emmc_cmd){ .index = 2, .flags = R2 };
	if ((r = emmc_host_send(&c)))
		return r;
	memcpy(card.cid, c.resp, sizeof card.cid);
	if ((r = cmd(3, RCA << 16, R1, NULL)))
		return r;
	card.rca = RCA;
	emmc_host_open_drain(false);
	c = (struct emmc_cmd){ .index = 9, .arg = RCA << 16, .flags = R2 };
	if ((r = emmc_host_send(&c)))
		return r;
	memcpy(card.csd, c.resp, sizeof card.csd);
	if ((r = cmd(7, RCA << 16, R1B, NULL)))
		return r;
	return wait_tran(1000);
}

static int card_speed_up(void)
{
	int r;
	if ((r = ext_csd_switch(SWITCH_WRITE_BYTE, EXT_CSD_HS_TIMING, 1))) {
		log_printf("emmc: HS_TIMING switch failed %d\n", r);
		return r;
	}
	emmc_host_high_speed();
	if ((r = emmc_host_clock(50000000, &card.clock_hz)))
		return r;
	card.high_speed = true;
	if ((r = wait_tran(1000)))
		return r;
	unsigned width = emmc_host_max_width();
	if ((r = ext_csd_switch(SWITCH_WRITE_BYTE, EXT_CSD_BUS_WIDTH, width == 8 ? 2 : 1))) {
		log_printf("emmc: BUS_WIDTH switch failed %d\n", r);
		return r;
	}
	emmc_host_set_width(width);
	card.bus_width = width;
	return 0;
}

static bool card_asleep;
static uint32_t last_io_ms;

int emmc_init(bool fast)
{
	static uint32_t generation;
	memset(&card, 0, sizeof card);
	card_asleep = false;
	card.generation = ++generation;
	nparts = 0;
	emmc_guard_clear();
	card.bus_width = 1;
	uint32_t t0 = time_ms();
	int r = emmc_host_init(&card);
	if (!r)
		r = card_identify();
	if (!r)
		r = read_ext_csd();
	if (!r && (card.ext_csd[EXT_CSD_PART_CONFIG] & 7)) {
		log_printf("emmc: partition access %u, switching to the user area\n", card.ext_csd[EXT_CSD_PART_CONFIG] & 7);
		r = ext_csd_switch(SWITCH_CLEAR_BITS, EXT_CSD_PART_CONFIG, 7);
	}
	if (!r && fast)
		r = card_speed_up();
	if (!r)
		r = read_ext_csd();
	if (!r) {
		const uint8_t *e = card.ext_csd;
		card.sectors = card.sector_mode ? (uint32_t)e[EXT_CSD_SEC_COUNT] | (uint32_t)e[EXT_CSD_SEC_COUNT + 1] << 8 |
							  (uint32_t)e[EXT_CSD_SEC_COUNT + 2] << 16 | (uint32_t)e[EXT_CSD_SEC_COUNT + 3] << 24
						: 0;
		if ((e[EXT_CSD_PART_CONFIG] & 7) || (fast && (e[EXT_CSD_HS_TIMING] != 1 || e[EXT_CSD_BUS_WIDTH] != (card.bus_width == 8 ? 2 : 1)))) {
			log_printf("emmc: ext_csd disagrees: part %02x hs %02x width %02x\n", e[EXT_CSD_PART_CONFIG], e[EXT_CSD_HS_TIMING],
				   e[EXT_CSD_BUS_WIDTH]);
			r = -6;
		}
		if (!card.sector_mode) {
			log_printf("emmc: byte-addressed card not supported\n");
			r = -7;
		}
	}
	card.ready = !r;
	log_printf("emmc: init %s in %lu ms: %lu sectors, %u-bit, %lu Hz\n", r ? "failed" : "done", time_ms() - t0, card.sectors,
		   card.bus_width, card.clock_hz);
	return r;
}

const struct emmc_info *emmc_card(void)
{
	return &card;
}

/* The card asleep as the vendor's suspend leaves it: cache flushed,
 * deselected, CMD5 sleep, the card's clock stopped and the host's clocks
 * off. Any transfer wakes it; a failed wake stops transfers. */
int emmc_sleep(bool on)
{
	if (!card.ready || on == card_asleep)
		return card.ready ? 0 : -1;
	int r;
	if (on) {
		if ((r = wait_tran(1000)))
			goto fail;
		if ((card.ext_csd[33] & 1) && (r = ext_csd_switch(SWITCH_WRITE_BYTE, 32, 1)))
			goto fail;
		if ((r = cmd(7, 0, R_NONE, NULL)))
			goto fail;
		if ((r = cmd(5, RCA << 16 | BIT(15), R1B, NULL))) {
			if (cmd(7, RCA << 16, R1B, NULL))
				card.ready = false;
			goto fail;
		}
		emmc_host_rest(true);
		card_asleep = true;
	} else {
		if ((r = emmc_host_rest(false))) {
			card.ready = false;
			goto fail;
		}
		card_asleep = false;
		if ((r = cmd(5, RCA << 16, R1B, NULL)) || (r = cmd(7, RCA << 16, R1B, NULL)) || (r = wait_tran(1000))) {
			card.ready = false;
			goto fail;
		}
	}
	log_printf("emmc: %s\n", on ? "asleep" : "awake");
	return 0;
fail:
	log_printf("emmc: %s failed (%d)%s\n", on ? "sleep" : "wake", r, card.ready ? "" : ", card disabled");
	return r;
}

/* While the watch sleeps, once nothing has used the card for a while. */
void emmc_idle(void)
{
	if (card.ready && !card_asleep && time_ms() - last_io_ms >= EMMC_IDLE_MS && emmc_sleep(true))
		last_io_ms = time_ms();
}

uint32_t emmc_next_ms(void)
{
	if (!card.ready || card_asleep)
		return UINT32_MAX;
	uint32_t gone = time_ms() - last_io_ms;
	return gone >= EMMC_IDLE_MS ? 1 : EMMC_IDLE_MS - gone;
}

static int xfer(uint32_t lba, uint32_t n, void *buf, bool read)
{
	if (!card.ready)
		return -1;
	if (card_asleep && emmc_sleep(false))
		return -1;
	last_io_ms = time_ms();
	if (!n || lba >= card.sectors || n > card.sectors - lba)
		return -2;
	uint8_t *p = buf;
	while (n) {
		uint32_t k = n < EMMC_XFER_BLOCKS ? n : EMMC_XFER_BLOCKS;
		if (!read)
			memcpy(emmc_bounce, p, k * EMMC_BLOCK);
		struct emmc_cmd c = { .index = read ? (k > 1 ? 18 : 17) : (k > 1 ? 25 : 24), .arg = lba, .flags = R1,
				 .blocks = k, .blksz = EMMC_BLOCK, .read = read, .timeout_us = 2000000 };
		int r = emmc_host_send(&c);
		if (!r && (c.resp[0] & STATUS_ERRORS)) {
			log_printf("emmc: cmd%lu lba %lu: status %08lx\n", c.index, lba, c.resp[0]);
			r = -3;
		}
		if (!r && !read)
			r = wait_tran(2000);
		if (r)
			return r;
		if (read)
			memcpy(p, emmc_bounce, k * EMMC_BLOCK);
		p += k * EMMC_BLOCK;
		lba += k;
		n -= k;
		wdt_feed();
	}
	return 0;
}

int emmc_read(uint32_t lba, uint32_t n, void *buf)
{
	int r = xfer(lba, n, buf, true);
	last_io_ms = time_ms();
	return r;
}

static bool guarded(uint32_t lba, uint32_t n)
{
	for (unsigned i = 0; i < nguard; i++)
		if (lba >= guard[i].first && n <= guard[i].count && lba - guard[i].first <= guard[i].count - n)
			return true;
	return false;
}

int emmc_write(uint32_t lba, uint32_t n, const void *buf)
{
	if (!n || !guarded(lba, n)) {
		log_printf("emmc: write of %lu blocks at %lu refused\n", n, lba);
		return -10;
	}
	int r = xfer(lba, n, (void *)(uintptr_t)buf, false);
	last_io_ms = time_ms();
	return r;
}

void emmc_guard_clear(void)
{
	nguard = 0;
	guard_first = guard_count = 0;
}

int emmc_guard_set(const struct emmc_range *r, unsigned n)
{
	emmc_guard_clear();
	if (!card.ready || n > MAX_GUARD || emmc_part_find(EMMC_WRITE_PARTITION, &guard_first, &guard_count))
		return -1;
	for (unsigned i = 0; i < n; i++) {
		if (!r[i].count || r[i].first < guard_first || r[i].count > guard_count ||
		    r[i].first - guard_first > guard_count - r[i].count) {
			log_printf("emmc: guard range %lu+%lu outside %s\n", r[i].first, r[i].count, EMMC_WRITE_PARTITION);
			return -2;
		}
	}
	memcpy(guard, r, n * sizeof *r);
	nguard = n;
	return 0;
}

static void utf16_name(char *dst, const uint8_t *src)
{
	unsigned i;
	for (i = 0; i < 36; i++) {
		uint16_t c = (uint16_t)(src[2 * i] | src[2 * i + 1] << 8);
		if (!c)
			break;
		dst[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
	}
	dst[i] = 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int gpt_load(uint32_t header_lba)
{
	uint8_t hdr[EMMC_BLOCK];
	int r = emmc_read(header_lba, 1, hdr);
	if (r)
		return r;
	if (memcmp(hdr, "EFI PART", 8))
		return -20;
	uint32_t hsize = le32(hdr + 12), hcrc = le32(hdr + 16);
	if (hsize < 92 || hsize > EMMC_BLOCK)
		return -21;
	uint8_t tmp[EMMC_BLOCK];
	memcpy(tmp, hdr, hsize);
	memset(tmp + 16, 0, 4);
	if (crc32(0, tmp, hsize) != hcrc)
		return -22;
	if (le32(hdr + 76) || le32(hdr + 24 + 4))
		return -23;
	uint32_t entries_lba = le32(hdr + 72), count = le32(hdr + 80), esize = le32(hdr + 84), ecrc = le32(hdr + 88);
	if (esize < 128 || esize > 512 || (esize & 7) || !count || count * esize > sizeof gpt_buf)
		return -24;
	uint32_t blocks = (count * esize + EMMC_BLOCK - 1) / EMMC_BLOCK;
	if ((r = emmc_read(entries_lba, blocks, gpt_buf)))
		return r;
	if (crc32(0, gpt_buf, count * esize) != ecrc)
		return -25;
	nparts = 0;
	for (uint32_t i = 0; i < count && nparts < MAX_PARTS; i++) {
		const uint8_t *e = gpt_buf + i * esize;
		static const uint8_t zero[16];
		if (!memcmp(e, zero, 16))
			continue;
		uint32_t first = le32(e + 32), last = le32(e + 40);
		if (le32(e + 36) || le32(e + 44) || last < first || last >= card.sectors)
			return -26;
		struct emmc_part *p = &parts[nparts++];
		utf16_name(p->name, e + 56);
		p->first = first;
		p->count = last - first + 1;
	}
	return 0;
}

int emmc_part_scan(void)
{
	nparts = 0;
	if (!card.ready)
		return -1;
	int r = gpt_load(1);
	if (r) {
		log_printf("emmc: primary GPT unusable (%d), trying the backup\n", r);
		r = gpt_load(card.sectors - 1);
	}
	if (r)
		nparts = 0;
	return r;
}

unsigned emmc_part_count(void) { return nparts; }

const struct emmc_part *emmc_part_get(unsigned i)
{
	return i < nparts ? &parts[i] : NULL;
}

int emmc_part_find(const char *name, uint32_t *first, uint32_t *count)
{
	for (unsigned i = 0; i < nparts; i++)
		if (!strcmp(parts[i].name, name)) {
			*first = parts[i].first;
			*count = parts[i].count;
			return 0;
		}
	return -1;
}
