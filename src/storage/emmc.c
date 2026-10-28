/* eMMC on SDCC1: the MSM SDHCI core (standard SDHCI registers at
 * 0xf9824900, the vendor "core" block with HC mode and the power-control
 * handshake at 0xf9824000). The card clock comes straight from the GCC
 * clock generator, the SDHCI divider is not used, and at 400 kHz status
 * clears need 40 us to land. The bootloader deselects the card, puts it to
 * sleep and turns HC mode off before it starts a kernel, so every init
 * starts from CMD0. Data moves by 32-bit ADMA2 through an aligned bounce
 * buffer, one descriptor per command. Writes are refused unless the whole
 * range lies inside one range of the write guard; the guard only accepts
 * ranges inside the EMMC_WRITE_PARTITION partition of the GPT. */
#include <string.h>
#include "asteroid.h"
#include "emmc.h"

#define GCC_BASE		0xfc400000u
#define GCC_GPLL0_STATUS	(GCC_BASE + 0x001c)
#define GCC_GPLL_ENA_VOTE	(GCC_BASE + 0x1480)
#define GCC_SDCC1_APPS_CBCR	(GCC_BASE + 0x04c4)
#define GCC_SDCC1_AHB_CBCR	(GCC_BASE + 0x04c8)
#define GCC_SDCC1_CMD_RCGR	(GCC_BASE + 0x04d0)
#define GCC_SDCC1_CFG_RCGR	(GCC_BASE + 0x04d4)
#define TLMM_SDC1_HDRV_PULL	(TLMM_BASE + 0x2044)

#define CORE			0xf9824000u
#define CORE_MCI_VERSION	(CORE + 0x050)
#define CORE_POWER		(CORE + 0x000)
#define CORE_SW_RST		BIT(7)
#define CORE_HC_MODE		(CORE + 0x078)
#define CORE_PWRCTL_STATUS	(CORE + 0x0dc)
#define CORE_PWRCTL_MASK	(CORE + 0x0e0)
#define CORE_PWRCTL_CLEAR	(CORE + 0x0e4)
#define CORE_PWRCTL_CTL		(CORE + 0x0e8)
#define HC_MODE_EN		BIT(0)
#define FF_CLK_SW_RST_DIS	BIT(13)
#define PWRCTL_BUS		(BIT(0) | BIT(1))
#define PWRCTL_IO		(BIT(2) | BIT(3))
#define PWRCTL_BUS_SUCCESS	BIT(0)
#define PWRCTL_IO_SUCCESS	BIT(2)

#define HC			0xf9824900u
#define HC_BLKSZ		0x004
#define HC_BLKCNT		0x006
#define HC_ARG			0x008
#define HC_XFER_MODE		0x00c
#define HC_CMD			0x00e
#define HC_RESP			0x010
#define HC_PRESENT		0x024
#define HC_CTRL1		0x028
#define HC_PWR			0x029
#define HC_CLK			0x02c
#define HC_TIMEOUT		0x02e
#define HC_RESET		0x02f
#define HC_NINT			0x030
#define HC_EINT			0x032
#define HC_NINT_EN		0x034
#define HC_EINT_EN		0x036
#define HC_NINT_SIG		0x038
#define HC_EINT_SIG		0x03a
#define HC_CTRL2		0x03e
#define HC_CAPS			0x040
#define HC_ADMA_ERR		0x054
#define HC_ADMA_ADDR		0x058
#define HC_VENDOR_SPEC		0x10c
#define HC_VENDOR_CAPS0		0x11c

#define RESET_ALL		BIT(0)
#define RESET_CMD_DAT		(BIT(1) | BIT(2))
#define CTRL1_8BIT		BIT(5)
#define CTRL1_ADMA32		(2u << 3)
#define CTRL2_UHS_MASK		7u
#define CTRL2_SDR25		1u
#define CLK_INT_EN		BIT(0)
#define CLK_STABLE		BIT(1)
#define CLK_SD_EN		BIT(2)
#define PRESENT_INHIBIT		(BIT(0) | BIT(1))
#define NINT_CMD_DONE		BIT(0)
#define NINT_XFER_DONE		BIT(1)
#define NINT_ERROR		BIT(15)
#define EINT_DATA_TIMEOUT	BIT(4)
#define CAPS_8BIT		BIT(18)
#define CAPS_3V3		BIT(24)
#define CAPS_3V0		BIT(25)
#define CAPS_1V8		BIT(26)
#define XFER_DMA		BIT(0)
#define XFER_BLKCNT		BIT(1)
#define XFER_AUTO_CMD12		BIT(2)
#define XFER_READ		BIT(4)
#define XFER_MULTI		BIT(5)
#define CMD_RESP_NONE		0u
#define CMD_RESP_136		1u
#define CMD_RESP_48		2u
#define CMD_RESP_48_BUSY	3u
#define CMD_CRC			BIT(3)
#define CMD_INDEX		BIT(4)
#define CMD_DATA		BIT(5)
#define ADMA_VALID		BIT(0)
#define ADMA_END		BIT(1)
#define ADMA_TRAN		(2u << 4)

#define R_NONE			CMD_RESP_NONE
#define R1			(CMD_RESP_48 | CMD_CRC | CMD_INDEX)
#define R1B			(CMD_RESP_48_BUSY | CMD_CRC | CMD_INDEX)
#define R2			(CMD_RESP_136 | CMD_CRC)
#define R3			CMD_RESP_48

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

#define MAX_XFER_BLOCKS		64u
#define MAX_PARTS		64u
#define MAX_GUARD		32u

static struct emmc_info card;
static struct emmc_part parts[MAX_PARTS];
static unsigned nparts;
static struct emmc_range guard[MAX_GUARD];
static unsigned nguard;
static uint32_t guard_first, guard_count;

static uint8_t bounce[MAX_XFER_BLOCKS * EMMC_BLOCK] __attribute__((aligned(64)));
static struct { uint16_t attr, len; uint32_t addr; } adma[8] __attribute__((aligned(64)));
static uint8_t gpt_buf[32 * EMMC_BLOCK] __attribute__((aligned(64)));

static inline uint8_t hc_rb(uint32_t r) { return *(volatile uint8_t *)(uintptr_t)(HC + r); }
static inline void hc_wb(uint8_t v, uint32_t r) { *(volatile uint8_t *)(uintptr_t)(HC + r) = v; }
static inline uint16_t hc_rw(uint32_t r) { return *(volatile uint16_t *)(uintptr_t)(HC + r); }
static inline void hc_ww(uint16_t v, uint32_t r) { *(volatile uint16_t *)(uintptr_t)(HC + r) = v; }
static inline uint32_t hc_rl(uint32_t r) { return readl(HC + r); }
static inline void hc_wl(uint32_t v, uint32_t r) { writel(v, HC + r); }

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

static bool wait_clear(uint32_t addr, uint32_t mask, uint32_t us)
{
	uint32_t t0 = time_us();
	while (readl(addr) & mask)
		if (time_us() - t0 > us)
			return false;
	return true;
}

static void gcc_enable(void)
{
	writel(readl(GCC_GPLL_ENA_VOTE) | BIT(0), GCC_GPLL_ENA_VOTE);
	uint32_t t0 = time_us();
	while (!(readl(GCC_GPLL0_STATUS) & BIT(17)) && time_us() - t0 < 1000)
		;
	writel(readl(GCC_SDCC1_AHB_CBCR) | BIT(0), GCC_SDCC1_AHB_CBCR);
	wait_clear(GCC_SDCC1_AHB_CBCR, BIT(31), 1000);
	writel(1u << 8 | (2 * 3 - 1), GCC_SDCC1_CFG_RCGR);
	writel(readl(GCC_SDCC1_CMD_RCGR) | BIT(0), GCC_SDCC1_CMD_RCGR);
	wait_clear(GCC_SDCC1_CMD_RCGR, BIT(0), 1000);
	writel(readl(GCC_SDCC1_APPS_CBCR) | BIT(0), GCC_SDCC1_APPS_CBCR);
	wait_clear(GCC_SDCC1_APPS_CBCR, BIT(31), 1000);
}

static void pins_config(void)
{
	uint32_t v = readl(TLMM_SDC1_HDRV_PULL);
	v &= ~0x7fffu;
	v |= 2u << 0 | 4u << 3 | 7u << 6 | 3u << 9 | 3u << 11 | 0u << 13;
	writel(v, TLMM_SDC1_HDRV_PULL);
}

static void hc_reset(uint8_t mask)
{
	hc_wb(mask, HC_RESET);
	uint32_t t0 = time_us();
	while ((hc_rb(HC_RESET) & mask) && time_us() - t0 < 100000)
		;
}

static int pwrctl_handshake(void)
{
	uint32_t t0 = time_us(), st;
	while (!((st = readl(CORE_PWRCTL_STATUS)) & 0xf))
		if (time_us() - t0 > 100000)
			return -1;
	writel(st, CORE_PWRCTL_CLEAR);
	uint32_t ack = 0;
	if (st & PWRCTL_BUS)
		ack |= PWRCTL_BUS_SUCCESS;
	if (st & PWRCTL_IO)
		ack |= PWRCTL_IO_SUCCESS;
	writel(ack, CORE_PWRCTL_CTL);
	dsb();
	return (int)st;
}

static void pwrctl_drain(void)
{
	uint32_t st = readl(CORE_PWRCTL_STATUS);
	writel(st, CORE_PWRCTL_CLEAR);
	uint32_t ack = readl(CORE_PWRCTL_CTL);
	if (st & PWRCTL_BUS)
		ack |= PWRCTL_BUS_SUCCESS;
	if (st & PWRCTL_IO)
		ack |= PWRCTL_IO_SUCCESS;
	writel(ack, CORE_PWRCTL_CTL);
	dsb();
}

static const struct {
	uint32_t hz, cfg, m, n;
} rates[] = {
	{ 400000, 0u << 8 | (2 * 12 - 1) | 2u << 12, 1, 4 },
	{ 50000000, 1u << 8 | (2 * 12 - 1), 0, 0 },
};

static int set_clock(uint32_t hz)
{
	unsigned i = 0;
	while (i < ARRAY_SIZE(rates) - 1 && rates[i].hz != hz)
		i++;
	if (hc_rl(HC_PRESENT) & PRESENT_INHIBIT)
		return -1;
	hc_ww(0, HC_CLK);
	writel(rates[i].m, GCC_SDCC1_CMD_RCGR + 0x8);
	writel(rates[i].n ? ~(rates[i].n - rates[i].m) : 0, GCC_SDCC1_CMD_RCGR + 0xc);
	writel(rates[i].n ? ~rates[i].n : 0, GCC_SDCC1_CMD_RCGR + 0x10);
	writel(rates[i].cfg, GCC_SDCC1_CFG_RCGR);
	writel(readl(GCC_SDCC1_CMD_RCGR) | BIT(0), GCC_SDCC1_CMD_RCGR);
	wait_clear(GCC_SDCC1_CMD_RCGR, BIT(0), 1000);
	hc_ww(CLK_INT_EN, HC_CLK);
	uint32_t t0 = time_us();
	while (!(hc_rw(HC_CLK) & CLK_STABLE))
		if (time_us() - t0 > 20000)
			return -2;
	hc_ww(CLK_INT_EN | CLK_SD_EN, HC_CLK);
	card.clock_hz = rates[i].hz;
	return 0;
}

struct cmd {
	uint32_t index, arg, flags;
	uint32_t resp[4];
	uint32_t blocks, blksz;
	bool read;
	uint32_t timeout_us;
};

static int cmd_fail(struct cmd *c, const char *what, uint16_t nint)
{
	uint16_t eint = hc_rw(HC_EINT);
	log_printf("emmc: cmd%lu arg %08lx %s: nint %04x eint %04x present %08lx adma %08lx\n", c->index, c->arg, what,
		   nint, eint, hc_rl(HC_PRESENT), hc_rl(HC_ADMA_ERR));
	hc_ww(0xffff, HC_NINT);
	hc_ww(0xffff, HC_EINT);
	hc_reset(RESET_CMD_DAT);
	return -(int)(eint ? eint : 0x10000);
}

static void slow_clear(void)
{
	if (card.clock_hz <= 400000)
		delay_us(40);
}

static int send(struct cmd *c)
{
	uint32_t t0 = time_us();
	while (hc_rl(HC_PRESENT) & PRESENT_INHIBIT) {
		if (time_us() - t0 > 100000) {
			log_printf("emmc: cmd%lu: lines busy %08lx\n", c->index, hc_rl(HC_PRESENT));
			hc_reset(RESET_CMD_DAT);
			return -1;
		}
	}
	hc_ww(0xffff, HC_NINT);
	hc_ww(0xffff, HC_EINT);
	slow_clear();
	hc_wb(0xe, HC_TIMEOUT);
	uint16_t mode = 0;
	uint32_t flags = c->flags;
	if (c->blocks) {
		uint32_t bytes = c->blocks * c->blksz;
		adma[0].addr = (uint32_t)(uintptr_t)bounce;
		adma[0].len = (uint16_t)bytes;
		adma[0].attr = ADMA_VALID | ADMA_END | ADMA_TRAN;
		dcache_clean_range(adma, sizeof adma);
		if (c->read)
			dcache_invalidate_range(bounce, bytes);
		else
			dcache_clean_range(bounce, bytes);
		hc_wl((uint32_t)(uintptr_t)adma, HC_ADMA_ADDR);
		hc_ww((uint16_t)c->blksz, HC_BLKSZ);
		hc_ww((uint16_t)c->blocks, HC_BLKCNT);
		mode = XFER_DMA | (c->read ? XFER_READ : 0);
		if (c->blocks > 1)
			mode |= XFER_MULTI | XFER_BLKCNT | XFER_AUTO_CMD12;
		flags |= CMD_DATA;
	}
	hc_wl(c->arg, HC_ARG);
	hc_ww(mode, HC_XFER_MODE);
	dsb();
	hc_ww((uint16_t)(c->index << 8 | flags), HC_CMD);
	uint16_t nint;
	t0 = time_us();
	while (!((nint = hc_rw(HC_NINT)) & (NINT_CMD_DONE | NINT_ERROR))) {
		if (time_us() - t0 > 100000)
			return cmd_fail(c, "no response", nint);
	}
	if (nint & NINT_ERROR)
		return cmd_fail(c, "error", nint);
	hc_ww(NINT_CMD_DONE, HC_NINT);
	slow_clear();
	if ((flags & 3) == CMD_RESP_136) {
		uint32_t r[4];
		for (int i = 0; i < 4; i++)
			r[i] = hc_rl(HC_RESP + i * 4);
		for (int i = 0; i < 4; i++)
			c->resp[i] = r[i] << 8 | (i ? r[i - 1] >> 24 : 0);
	} else {
		c->resp[0] = hc_rl(HC_RESP);
	}
	if (c->blocks || (flags & 3) == CMD_RESP_48_BUSY) {
		uint32_t limit = c->timeout_us ? c->timeout_us : 1000000;
		t0 = time_us();
		for (;;) {
			nint = hc_rw(HC_NINT);
			if (nint & NINT_XFER_DONE)
				break;
			if ((nint & NINT_ERROR) && !(hc_rw(HC_EINT) & ~EINT_DATA_TIMEOUT))
				break;
			if (nint & NINT_ERROR)
				return cmd_fail(c, "transfer error", nint);
			if (time_us() - t0 > limit)
				return cmd_fail(c, "transfer timeout", nint);
			wdt_feed();
		}
		if (!(nint & NINT_XFER_DONE))
			return cmd_fail(c, "data timeout", nint);
		hc_ww(NINT_XFER_DONE, HC_NINT);
		if (c->blocks && c->read)
			dcache_invalidate_range(bounce, c->blocks * c->blksz);
	}
	return 0;
}

static int cmd(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *resp)
{
	struct cmd c = { .index = index, .arg = arg, .flags = flags };
	int r = send(&c);
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
	struct cmd c = { .index = 8, .flags = R1, .blocks = 1, .blksz = 512, .read = true };
	int r = send(&c);
	if (!r)
		memcpy(card.ext_csd, bounce, 512);
	return r;
}

static void rails_on(void)
{
	uint32_t l17[6] = { 0x6e657773, 4, 1, 0x7675, 4, 2950000 };
	uint32_t l6[6] = { 0x6e657773, 4, 1, 0x7675, 4, 1800000 };
	rpm_request(RPM_LDOA, 17, l17, 2);
	rpm_request(RPM_LDOA, 6, l6, 2);
}

static int host_init(void)
{
	rails_on();
	gcc_enable();
	pins_config();
	writel(0, CORE_HC_MODE);
	writel(readl(CORE_POWER) | CORE_SW_RST, CORE_POWER);
	wait_clear(CORE_POWER, CORE_SW_RST, 1000);
	writel(HC_MODE_EN | FF_CLK_SW_RST_DIS, CORE_HC_MODE);
	hc_wl(0xa1c, HC_VENDOR_SPEC);
	dsb();
	hc_reset(RESET_ALL);
	pwrctl_drain();
	writel(0xf, CORE_PWRCTL_MASK);
	card.mci_version = readl(CORE_MCI_VERSION);
	uint32_t major = card.mci_version >> 28, minor = card.mci_version & 0xff;
	uint32_t caps = hc_rl(HC_CAPS);
	if (major >= 1 && minor != 0x11 && minor != 0x12)
		hc_wl(caps | CAPS_1V8, HC_VENDOR_CAPS0);
	card.caps = caps = hc_rl(HC_CAPS);
	uint8_t vol = (caps & CAPS_3V3) ? 7 : (caps & CAPS_3V0) ? 6 : 5;
	hc_wb((uint8_t)(vol << 1), HC_PWR);
	hc_wb((uint8_t)(vol << 1 | 1), HC_PWR);
	int pw = pwrctl_handshake();
	log_printf("emmc: mci %08lx caps %08lx, bus power %u, pwrctl %d\n", card.mci_version, caps, vol, pw);
	if (pw < 0)
		return -1;
	hc_wb(CTRL1_ADMA32, HC_CTRL1);
	hc_ww(0, HC_CTRL2);
	hc_ww(0x000b, HC_NINT_EN);
	hc_ww(0xffff, HC_EINT_EN);
	hc_ww(0, HC_NINT_SIG);
	hc_ww(0, HC_EINT_SIG);
	if (set_clock(400000))
		return -2;
	delay_ms(1);
	return 0;
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
	struct cmd c;
	uint32_t ocr = 0;
	int r = card_ready(&ocr);
	if (r) {
		log_printf("emmc: no answer to CMD1 (%d), waking the card from sleep\n", r);
		cmd(5, RCA << 16, R1B, NULL);
		if ((r = card_ready(&ocr)))
			return r;
	}
	card.ocr = ocr;
	card.sector_mode = ocr & OCR_SECTOR;
	c = (struct cmd){ .index = 2, .flags = R2 };
	if ((r = send(&c)))
		return r;
	memcpy(card.cid, c.resp, sizeof card.cid);
	if ((r = cmd(3, RCA << 16, R1, NULL)))
		return r;
	card.rca = RCA;
	c = (struct cmd){ .index = 9, .arg = RCA << 16, .flags = R2 };
	if ((r = send(&c)))
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
	hc_ww((uint16_t)((hc_rw(HC_CTRL2) & ~CTRL2_UHS_MASK) | CTRL2_SDR25), HC_CTRL2);
	if ((r = set_clock(50000000)))
		return r;
	card.high_speed = true;
	if ((r = wait_tran(1000)))
		return r;
	unsigned width = (card.caps & CAPS_8BIT) ? 8 : 4;
	if ((r = ext_csd_switch(SWITCH_WRITE_BYTE, EXT_CSD_BUS_WIDTH, width == 8 ? 2 : 1))) {
		log_printf("emmc: BUS_WIDTH switch failed %d\n", r);
		return r;
	}
	hc_wb((uint8_t)((hc_rb(HC_CTRL1) & ~(CTRL1_8BIT | BIT(1))) | (width == 8 ? CTRL1_8BIT : BIT(1))), HC_CTRL1);
	card.bus_width = width;
	return 0;
}

int emmc_init(bool fast)
{
	static uint32_t generation;
	memset(&card, 0, sizeof card);
	card.generation = ++generation;
	nparts = 0;
	emmc_guard_clear();
	card.bus_width = 1;
	uint32_t t0 = time_ms();
	int r = host_init();
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

static int xfer(uint32_t lba, uint32_t n, void *buf, bool read)
{
	if (!card.ready)
		return -1;
	if (!n || lba >= card.sectors || n > card.sectors - lba)
		return -2;
	uint8_t *p = buf;
	while (n) {
		uint32_t k = n < MAX_XFER_BLOCKS ? n : MAX_XFER_BLOCKS;
		if (!read)
			memcpy(bounce, p, k * EMMC_BLOCK);
		struct cmd c = { .index = read ? (k > 1 ? 18 : 17) : (k > 1 ? 25 : 24), .arg = lba, .flags = R1,
				 .blocks = k, .blksz = EMMC_BLOCK, .read = read, .timeout_us = 2000000 };
		int r = send(&c);
		if (!r && (c.resp[0] & STATUS_ERRORS)) {
			log_printf("emmc: cmd%lu lba %lu: status %08lx\n", c.index, lba, c.resp[0]);
			r = -3;
		}
		if (!r && !read)
			r = wait_tran(2000);
		if (r)
			return r;
		if (read)
			memcpy(p, bounce, k * EMMC_BLOCK);
		p += k * EMMC_BLOCK;
		lba += k;
		n -= k;
		wdt_feed();
	}
	return 0;
}

int emmc_read(uint32_t lba, uint32_t n, void *buf)
{
	return xfer(lba, n, buf, true);
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
	return xfer(lba, n, (void *)(uintptr_t)buf, false);
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
