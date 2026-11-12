/* The eMMC's host on the Qualcomm chips, SDCC1: the MSM SDHCI core (standard
 * SDHCI registers 0x900 into the block, the vendor "core" block with HC
 * mode and the power-control handshake at its start). The card clock comes
 * straight from the GCC clock generator, the SDHCI divider is not used,
 * and at 400 kHz status clears need 40 us to land. The bootloader
 * deselects the card, puts it to sleep and turns HC mode off before it
 * starts a kernel. Data moves by 32-bit ADMA2 through the aligned bounce
 * buffer, one descriptor per command. */
#include <string.h>
#include "asteroid.h"
#include "emmc_host.h"

#define GCC_GPLL0_STATUS	GCC(chip->gpll0_status)
#define GCC_GPLL_ENA_VOTE	GCC(chip->gpll0_vote)
#define GCC_SDCC1_APPS_CBCR	GCC(chip->sdcc1_apps_cbcr)
#define GCC_SDCC1_AHB_CBCR	GCC(chip->sdcc1_ahb_cbcr)
#define GCC_SDCC1_CMD_RCGR	GCC(chip->sdcc1_rcgr)
#define GCC_SDCC1_CFG_RCGR	(GCC_SDCC1_CMD_RCGR + 4)
#define TLMM_SDC1_HDRV_PULL	(chip->sdc1_pads)

#define CORE			(chip->sdcc1)
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

#define HC			(chip->sdcc1 + 0x900)
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
#define ADMA_VALID		BIT(0)
#define ADMA_END		BIT(1)
#define ADMA_TRAN		(2u << 4)


static struct { uint16_t attr, len; uint32_t addr; } adma[8] __attribute__((aligned(64)));
static uint32_t clock_hz;
static bool caps8;

static inline uint8_t hc_rb(uint32_t r) { return *(volatile uint8_t *)(uintptr_t)(HC + r); }
static inline void hc_wb(uint8_t v, uint32_t r) { *(volatile uint8_t *)(uintptr_t)(HC + r) = v; }
static inline uint16_t hc_rw(uint32_t r) { return *(volatile uint16_t *)(uintptr_t)(HC + r); }
static inline void hc_ww(uint16_t v, uint32_t r) { *(volatile uint16_t *)(uintptr_t)(HC + r) = v; }
static inline uint32_t hc_rl(uint32_t r) { return readl(HC + r); }
static inline void hc_wl(uint32_t v, uint32_t r) { writel(v, HC + r); }

static bool wait_clear(uint32_t addr, uint32_t mask, uint32_t us)
{
	uint32_t t0 = time_us();
	while (readl(addr) & mask)
		if (time_us() - t0 > us)
			return false;
	return true;
}

/* A rate of GPLL0's, which runs at 600 MHz on the APQ8026 and 800 MHz on
 * the APQ8009W. */
static uint32_t gpll0_cfg(uint32_t hz)
{
	return 1u << 8 | (2 * (chip->gpll0_khz / (hz / 1000)) - 1);
}

static void gcc_enable(void)
{
	writel(readl(GCC_GPLL_ENA_VOTE) | BIT(0), GCC_GPLL_ENA_VOTE);
	uint32_t t0 = time_us();
	while (!(readl(GCC_GPLL0_STATUS) & chip->gpll0_ready) && time_us() - t0 < 1000)
		;
	writel(readl(GCC_SDCC1_AHB_CBCR) | BIT(0), GCC_SDCC1_AHB_CBCR);
	wait_clear(GCC_SDCC1_AHB_CBCR, BIT(31), 1000);
	writel(gpll0_cfg(200000000), GCC_SDCC1_CFG_RCGR);
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
	{ 50000000, 0, 0, 0 },
};

int emmc_host_clock(uint32_t hz, uint32_t *actual)
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
	writel(rates[i].cfg ? rates[i].cfg : gpll0_cfg(rates[i].hz), GCC_SDCC1_CFG_RCGR);
	writel(readl(GCC_SDCC1_CMD_RCGR) | BIT(0), GCC_SDCC1_CMD_RCGR);
	wait_clear(GCC_SDCC1_CMD_RCGR, BIT(0), 1000);
	hc_ww(CLK_INT_EN, HC_CLK);
	uint32_t t0 = time_us();
	while (!(hc_rw(HC_CLK) & CLK_STABLE))
		if (time_us() - t0 > 20000)
			return -2;
	hc_ww(CLK_INT_EN | CLK_SD_EN, HC_CLK);
	*actual = clock_hz = rates[i].hz;
	return 0;
}

static int cmd_fail(struct emmc_cmd *c, const char *what, uint16_t nint)
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
	if (clock_hz <= 400000)
		delay_us(40);
}

int emmc_host_send(struct emmc_cmd *c)
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
		adma[0].addr = dma_addr(emmc_bounce);
		adma[0].len = (uint16_t)bytes;
		adma[0].attr = ADMA_VALID | ADMA_END | ADMA_TRAN;
		dcache_clean_range(adma, sizeof adma);
		if (c->read)
			dcache_invalidate_range(emmc_bounce, bytes);
		else
			dcache_clean_range(emmc_bounce, bytes);
		hc_wl(dma_addr(adma), HC_ADMA_ADDR);
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
			dcache_invalidate_range(emmc_bounce, c->blocks * c->blksz);
	}
	return 0;
}

static void rails_on(void)
{
	uint32_t vcc[6] = { 0x6e657773, 4, 1, 0x7675, 4, chip->emmc_vcc.uv };
	uint32_t vccq[6] = { 0x6e657773, 4, 1, 0x7675, 4, chip->emmc_vccq.uv };
	rpm_request(RPM_LDOA, chip->emmc_vcc.ldo, vcc, 2);
	rpm_request(RPM_LDOA, chip->emmc_vccq.ldo, vccq, 2);
}

int emmc_host_init(struct emmc_info *card)
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
	card->mci_version = readl(CORE_MCI_VERSION);
	uint32_t major = card->mci_version >> 28, minor = card->mci_version & 0xff;
	uint32_t caps = hc_rl(HC_CAPS);
	if (major >= 1 && minor != 0x11 && minor != 0x12)
		hc_wl(caps | CAPS_1V8, HC_VENDOR_CAPS0);
	card->caps = caps = hc_rl(HC_CAPS);
	uint8_t vol = (caps & CAPS_3V3) ? 7 : (caps & CAPS_3V0) ? 6 : 5;
	hc_wb((uint8_t)(vol << 1), HC_PWR);
	hc_wb((uint8_t)(vol << 1 | 1), HC_PWR);
	int pw = pwrctl_handshake();
	log_printf("emmc: mci %08lx caps %08lx, bus power %u, pwrctl %d\n", card->mci_version, caps, vol, pw);
	if (pw < 0)
		return -1;
	hc_wb(CTRL1_ADMA32, HC_CTRL1);
	hc_ww(0, HC_CTRL2);
	hc_ww(0x000b, HC_NINT_EN);
	hc_ww(0xffff, HC_EINT_EN);
	hc_ww(0, HC_NINT_SIG);
	hc_ww(0, HC_EINT_SIG);
	caps8 = caps & CAPS_8BIT;
	if (emmc_host_clock(400000, &card->clock_hz))
		return -2;
	delay_ms(1);
	return 0;
}

void emmc_host_open_drain(bool on) {}

void emmc_host_high_speed(void)
{
	hc_ww((uint16_t)((hc_rw(HC_CTRL2) & ~CTRL2_UHS_MASK) | CTRL2_SDR25), HC_CTRL2);
}

unsigned emmc_host_max_width(void)
{
	return caps8 ? 8 : 4;
}

void emmc_host_set_width(unsigned width)
{
	hc_wb((uint8_t)((hc_rb(HC_CTRL1) & ~(CTRL1_8BIT | BIT(1))) | (width == 8 ? CTRL1_8BIT : BIT(1))), HC_CTRL1);
}

static bool branch_on(uint32_t cbcr)
{
	writel(readl(cbcr) | BIT(0), cbcr);
	return wait_clear(cbcr, BIT(31), 1000);
}

/* Waking only turns the branch clocks on again: the rate emmc_host_clock
 * chose stays programmed. */
int emmc_host_rest(bool on)
{
	if (on) {
		hc_ww((uint16_t)(hc_rw(HC_CLK) & ~CLK_SD_EN), HC_CLK);
		writel(readl(GCC_SDCC1_APPS_CBCR) & ~BIT(0), GCC_SDCC1_APPS_CBCR);
		writel(readl(GCC_SDCC1_AHB_CBCR) & ~BIT(0), GCC_SDCC1_AHB_CBCR);
		return 0;
	}
	if (!branch_on(GCC_SDCC1_AHB_CBCR) || !branch_on(GCC_SDCC1_APPS_CBCR))
		return -10;
	hc_ww((uint16_t)(hc_rw(HC_CLK) | CLK_SD_EN), HC_CLK);
	return 0;
}
