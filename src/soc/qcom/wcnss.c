/* The APQ8009W's own WCNSS core (Pronto) with its WCN3620A RF chip, as
 * swift's vendor kernel starts it: the supplies and the RF chip's
 * crystal set up from here, then the secure world checks the signed
 * firmware from the system partition and starts the core. The core
 * says it is up through smp2p and opens its control channel over SMD;
 * on it we ask for the version, then hand over the NV file, after
 * which the core calibrates its radio.
 *
 * Only a console command runs this, one stage after the other, and it
 * says how far it got. Unless told to keep it, the core is stopped
 * again at the end. Nothing waits on an interrupt: the core's state is
 * polled from SMEM, the watchdog fed meanwhile. */
#include <stdarg.h>
#include <string.h>
#include "asteroid.h"
#include "soc/qcom/mdt.h"
#include "soc/qcom/smd.h"
#include "soc/qcom/smem_find.h"
#include "soc/qcom/wcnss_msg.h"
#include "bt/bt.h"
#include "sched.h"

#define IO(pa)			(IO_8009_BASE + (pa))
#define PAS_WCNSS		6
#define FW			"/etc/firmware/wcnss"
#define NV			"/etc/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin"
#define REGION			((uint8_t *)WCNSS_8009_BASE)
#define REGION_SIZE		(WCNSS_8009_END - WCNSS_8009_BASE)
#define SMEM_SIZE		(SMEM_8009_END - SMEM_8009_BASE)

/* The core's power and clock unit, in its PHY block. */
#define PRONTO			IO(0x0a21b000)
#define PMU_CFG			(PRONTO + 0x1004)
#define PMU_SPARE		(PRONTO + 0x1088)
#define IRIS_READ		(PRONTO + 0x1134)
#define SPARE_NV_DOWNLOAD	BIT(25)
#define CFG_XO_MODE		(3u << 1)
#define CFG_XO_CFG		BIT(3)
#define CFG_XO_EN		BIT(4)
#define CFG_BUS_MUX_TOP		BIT(5)
#define CFG_XO_CFG_STS		BIT(6)
#define CFG_IRIS_RESET		BIT(7)
#define CFG_IRIS_RESET_STS	BIT(8)
#define CFG_XO_READ		BIT(9)
#define CFG_XO_READ_STS		BIT(10)
#define IRIS_CHIP_ID		0x04

/* The crypto engine the secure world checks the image with, at 80 MHz
 * from GPLL0, and its three branch votes. */
#define CRYPTO_CMD_RCGR		GCC(0x16004)
#define CRYPTO_CFG_RCGR		GCC(0x16008)
#define CRYPTO_CBCR		GCC(0x1601c)
#define BRANCH_ENA_VOTE		GCC(0x45004)
#define CRYPTO_80MHZ		(1u << 8 | (2 * 10 - 1))
#define CRYPTO_VOTES		7u
#define CBCR_OFF		BIT(31)

/* RPM: the supplies of the core and the RF chip, and the RF chip's
 * crystal buffer. */
#define RPM_CLKA		0x616b6c63u
#define RPM_KEY_SWEN		0x6e657773u
#define RF_CLK2			5
#define LDO_PX			7	/* also the crystal and the PLL, 1.8 V */
#define LDO_DIG			5	/* the RF chip's logic, 1.8 V; the eMMC's I/O too */
#define LDO_PA			9	/* the RF power amplifier, 3.3 V */
#define SMPS_RFA		3	/* the RF chip, 1.3 V */

#define SMD_CHANNELS		64
#define SMEM_PRONTO_REASON	422
#define SMEM_SMP2P_WCNSS	451
#define EDGE_APPS_WCNSS		6
#define XFER_PACKET		2
#define KICK_SMD		BIT(17)

#define SMP2P_MAGIC		0x504d5324u
#define SLAVE_ERR_FATAL		BIT(0)
#define SLAVE_ERR_READY		BIT(1)
#define SLAVE_PROXY_UNVOTE	BIT(2)
#define SMP2P_VERSION		1
#define SMP2P_PID_APPS		0
#define SMP2P_PID_WCNSS		4
#define SMP2P_ENTRIES_MAX	16
#define SMEM_SMP2P_APPS		431
#define SMP2P_ITEM_SIZE		(20 + SMP2P_ENTRIES_MAX * 20)
#define SMP2P_SSR_ACK		1
#define SMP2P_HEADER		(SMP2P_SSR_ACK << 8 | SMP2P_VERSION)
#define RESTART_DONE		BIT(0)
#define RESTART_ACK		BIT(1)
#define KICK_SMP2P		BIT(18)
#define SMEM_ALLOC_LOCK		IO(0x01905000 + 3 * 0x1000)
#define LOCK_APPS		1
#define LOCK_MS			100

#define READY_MS		10000
#define CHANNEL_MS		3000
#define OPEN_MS			2000
#define REPLY_MS		2000
#define ROOM_MS			1000
#define NV_MS			5000
#define CBC_MS			10000
#define CAL_MAX			(64 * 1024)

static void (*say)(const char *fmt, ...);
/* What the secure world was last told: nothing, an image accepted, the
 * core started, or a stop that failed, after which the core may still
 * run in its memory and nothing is loaded or switched off again. */
enum core { CORE_OFF, CORE_LOADING, CORE_RUNNING, CORE_UNKNOWN };
static enum core core;
/* What the probe itself switched on, to switch off again. */
static struct {
	bool pins, pa, rfa, rf_clock;
	uint32_t crypto_votes;
} ours;
static struct smd_chan ctrl;
static uint8_t mdt[8192] __attribute__((aligned(4096)));

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	log_puts(buf);
	if (say)
		say("%s", buf);
}

static volatile void *smem_get(unsigned id, int host, uint32_t *size)
{
	struct smem_loc l;
	if (smem_find((const volatile void *)(uintptr_t)chip->smem, SMEM_SIZE, id, host, &l) || l.base_ext)
		return NULL;
	if (size)
		*size = l.size;
	return (volatile uint8_t *)(uintptr_t)chip->smem + l.offset;
}

static bool wait_clear(uint32_t addr, uint32_t bits, uint32_t us)
{
	uint32_t t0 = time_us();
	while (readl(addr) & bits)
		if (time_us() - t0 > us)
			return false;
	return true;
}

static void wait_ms(uint32_t ms)
{
	for (; ms; ms -= ms < 10 ? ms : 10) {
		delay_ms(ms < 10 ? ms : 10);
		wdt_feed();
	}
}

static int smps_on(uint32_t id, uint32_t uv)
{
	int r = rpm_regulator_voltage(RPM_SMPA, id, uv);
	return r ? r : rpm_regulator_enable(RPM_SMPA, id, true);
}

static int rf_clock(bool on)
{
	uint32_t kvp[3] = { RPM_KEY_SWEN, 4, on };
	return rpm_request(RPM_CLKA, RF_CLK2, kvp, 1);
}

static bool crypto_clocks(bool on)
{
	uint32_t v = readl(BRANCH_ENA_VOTE);
	if (!on) {
		writel(v & ~ours.crypto_votes, BRANCH_ENA_VOTE);
		ours.crypto_votes = 0;
		return true;
	}
	writel(CRYPTO_80MHZ, CRYPTO_CFG_RCGR);
	writel(readl(CRYPTO_CMD_RCGR) | 1, CRYPTO_CMD_RCGR);
	if (!wait_clear(CRYPTO_CMD_RCGR, 1, 1000))
		return false;
	ours.crypto_votes |= CRYPTO_VOTES & ~v;
	writel(v | CRYPTO_VOTES, BRANCH_ENA_VOTE);
	return wait_clear(CRYPTO_CBCR, CBCR_OFF, 1000);
}

/* The supplies in the vendor's order, the core's then the RF chip's, and
 * the pins to the RF chip. */
static bool power_on(void)
{
	int fails = !!regulator_on(LDO_PX, 1800000);
	fails += !!regulator_on(LDO_DIG, 1800000);
	int r = regulator_on(LDO_PA, 3300000);
	ours.pa = true;
	fails += !!r;
	r = smps_on(SMPS_RFA, 1300000);
	ours.rfa = true;
	fails += !!r;
	for (unsigned g = 40; g <= 44; g++)
		gpio_config(g, 1, GPIO_INPUT, GPIO_PULL_UP, 6);
	ours.pins = true;
	bool crypto = crypto_clocks(true);
	note("wcnss: supplies on, %d not acknowledged; crypto clock %s (votes %lx ours)\n", fails, crypto ? "on" : "not on",
	     ours.crypto_votes);
	return !fails && crypto;
}

static bool iris_reset(uint32_t reg)
{
	writel(reg | CFG_IRIS_RESET, PMU_CFG);
	bool done = wait_clear(PMU_CFG, CFG_IRIS_RESET_STS, 10000);
	writel(reg & ~CFG_IRIS_RESET, PMU_CFG);
	return done;
}

static bool iris_valid(uint32_t id)
{
	switch (id >> 16) {
	case 0x0200: case 0x0300: case 0x0400: case 0x5111: case 0x5112: case 0x9101: case 0x9110:
		return true;
	}
	return false;
}

/* The RF chip's id read through the PMU, then its crystal mode set for
 * the 19.2 MHz the WCN3620 takes, with the RPM's buffer for it on. */
static bool xo_setup(void)
{
	writel(readl(PMU_SPARE) | SPARE_NV_DOWNLOAD, PMU_SPARE);
	writel(0, PMU_CFG);
	uint32_t reg = readl(PMU_CFG) | CFG_BUS_MUX_TOP | CFG_XO_EN;
	writel(reg, PMU_CFG);
	uint32_t iris = readl(IRIS_READ);
	unsigned stuck = !iris_reset(reg);
	writel((iris & 0xffff) | IRIS_CHIP_ID, IRIS_READ);
	bool valid = false;
	for (unsigned i = 0; i < 6 && !valid; i++) {
		writel(reg | CFG_XO_READ, PMU_CFG);
		bool read = wait_clear(PMU_CFG, CFG_XO_READ_STS, 10000);
		iris = readl(IRIS_READ);
		valid = read && iris_valid(iris);
		writel(reg, PMU_CFG);
		if (!valid)
			stuck += !iris_reset(reg);
	}
	if (valid && iris >> 30 == 0)
		reg = (reg & ~CFG_XO_MODE) | CFG_XO_MODE;
	else
		reg &= ~CFG_XO_MODE;
	writel(reg, PMU_CFG);
	stuck += !iris_reset(reg);
	writel(reg | CFG_XO_CFG, PMU_CFG);
	bool done = wait_clear(PMU_CFG, CFG_XO_CFG_STS, 100000);
	writel(reg & ~(CFG_BUS_MUX_TOP | CFG_XO_CFG), PMU_CFG);
	int rf = 0;
	if (!(iris >> 30 == 0 && valid)) {
		rf = rf_clock(true);
		ours.rf_clock = true;
	}
	wait_ms(20);
	note("wcnss: RF chip id %08lx (%s), crystal %s MHz, setup %s, %u resets timed out, RF clock %s\n", iris,
	     valid ? "known" : "unknown", iris >> 30 == 0 && valid ? "48" : "19.2", done ? "done" : "timed out", stuck,
	     rf ? "not acknowledged" : "on");
	return valid && done && !stuck && !rf;
}

/* Each segment from its own file into the region, the rest of it
 * zeroed, then written back from the cache for the secure world. */
static bool load(const struct mdt_image *img)
{
	uint32_t t0 = time_ms(), bytes = 0;
	for (unsigned i = 0; i < img->nsegs; i++) {
		const struct mdt_seg *s = &img->seg[i];
		uint8_t *dst = REGION + s->offset;
		if (s->filesz) {
			char path[40];
			snprintf(path, sizeof path, FW ".b%02u", s->num);
			int n = firmware_read(path, 0, dst, s->filesz);
			if (n != (int)s->filesz) {
				note("wcnss: %s: %d of %lu bytes\n", path, n, s->filesz);
				return false;
			}
			bytes += s->filesz;
		}
		memset(dst + s->filesz, 0, s->memsz - s->filesz);
		dcache_clean_invalidate_range(dst, s->memsz);
		wdt_feed();
	}
	note("wcnss: %u segments, %lu bytes loaded in %lu ms\n", img->nsegs, bytes, time_ms() - t0);
	return true;
}

static void shutdown(void)
{
	note("wcnss: shutdown...\n");
	int r = scm_pas_shutdown(PAS_WCNSS);
	core = r ? CORE_UNKNOWN : CORE_OFF;
	note("wcnss: shutdown %d%s\n", r, r ? ", core state unknown: supplies kept, no new load until a reboot" : "");
}

/* Only what the probe switched on, and the pins back to the vendor's
 * sleep setting; L5 and L7 have other users and stay. A vote the RPM
 * did not take stays marked, for the next stop to try again. */
static void power_off(void)
{
	if (core != CORE_OFF || !(ours.pins || ours.pa || ours.rfa || ours.rf_clock || ours.crypto_votes))
		return;
	int fails = 0;
	crypto_clocks(false);
	if (ours.rf_clock && !rf_clock(false))
		ours.rf_clock = false;
	if (ours.pa && !regulator_off(LDO_PA))
		ours.pa = false;
	if (ours.rfa && !rpm_regulator_enable(RPM_SMPA, SMPS_RFA, false))
		ours.rfa = false;
	fails = ours.rf_clock + ours.pa + ours.rfa;
	if (ours.pins) {
		for (unsigned g = 40; g <= 44; g++)
			gpio_config(g, 1, GPIO_INPUT, GPIO_PULL_DOWN, 2);
		ours.pins = false;
	}
	note("wcnss: crypto and RF clocks, PA and RF supplies off, %d not acknowledged\n", fails);
}

/* The secure world's part: the headers and hash table, the region, the
 * segments, then the check and the start. */
static bool boot(void)
{
	int n = firmware_read(FW ".mdt", 0, mdt, sizeof mdt);
	struct mdt_image img;
	int r = n > 0 && n < (int)sizeof mdt ? mdt_parse(mdt, (uint32_t)n, REGION_SIZE, &img) : -10;
	if (r) {
		note("wcnss: " FW ".mdt: %d bytes, not usable (%d)\n", n, r);
		return false;
	}
	note("wcnss: wcnss.mdt %d bytes, %u segments, %08lx + %06lx, entry %08lx\n", n, img.nsegs, img.base, img.size,
	     img.entry);
	dcache_clean_range(mdt, (uint32_t)n);
	note("wcnss: init image...\n");
	uint32_t t0 = time_ms();
	r = scm_pas_init_image(PAS_WCNSS, dma_addr(mdt));
	note("wcnss: init image %d in %lu ms\n", r, time_ms() - t0);
	if (r) {
		if (scm_pas_stuck())
			core = CORE_UNKNOWN;
		return false;
	}
	core = CORE_LOADING;
	uint32_t phys = dma_addr(REGION);
	note("wcnss: memory setup...\n");
	r = scm_pas_mem_setup(PAS_WCNSS, phys, img.size);
	note("wcnss: memory setup %08lx + %06lx: %d\n", phys, img.size, r);
	if (r || !load(&img))
		return false;
	note("wcnss: auth and reset...\n");
	t0 = time_ms();
	r = scm_pas_auth_and_reset(PAS_WCNSS);
	note("wcnss: auth and reset %d in %lu ms\n", r, time_ms() - t0);
	if (!r)
		core = CORE_RUNNING;
	return !r;
}

static void kick_smp2p(void)
{
	dsb();
	writel(KICK_SMP2P, APCS_IPC);
}

/* The hardware mutex the vendor's SMEM allocator takes; the core may add
 * items to the partition we share. */
static bool smem_lock(void)
{
	uint32_t t0 = time_ms();
	do {
		writel(LOCK_APPS, SMEM_ALLOC_LOCK);
		if (readl(SMEM_ALLOC_LOCK) == LOCK_APPS)
			return true;
	} while (time_ms() - t0 < LOCK_MS);
	return false;
}

static void smem_unlock(void)
{
	dsb();
	writel(0, SMEM_ALLOC_LOCK);
}

/* Our smp2p item to the core, which the vendor kernel has in place long
 * before it starts the core: version 1 with the restart ack, as the core
 * offers, and no entries. The version goes in last, since the core takes
 * an item without one as still being set up. An item already right is
 * kept, with the ack in it. */
static volatile uint32_t *outbound;

static void smp2p_open(void)
{
	if (outbound)
		return;
	struct smem_loc l;
	if (!smem_lock()) {
		note("wcnss: smem lock held by another processor\n");
		return;
	}
	int r = smem_alloc((volatile void *)(uintptr_t)chip->smem, SMEM_SIZE, SMEM_SMP2P_APPS, SMEM_HOST_WCNSS,
			   SMP2P_ITEM_SIZE, &l);
	smem_unlock();
	if (r || l.base_ext || l.size < SMP2P_ITEM_SIZE) {
		note("wcnss: no smp2p item of ours (%d, %lu bytes)\n", r, l.size);
		return;
	}
	volatile uint32_t *h = (volatile uint32_t *)((volatile uint8_t *)(uintptr_t)chip->smem + l.offset);
	bool kept = h[0] == SMP2P_MAGIC && h[1] == SMP2P_HEADER &&
		    h[2] == ((uint32_t)SMP2P_PID_WCNSS << 16 | SMP2P_PID_APPS);
	if (!kept) {
		h[1] = 0;
		dsb();
		h[0] = SMP2P_MAGIC;
		h[2] = (uint32_t)SMP2P_PID_WCNSS << 16 | SMP2P_PID_APPS;
		h[3] = SMP2P_ENTRIES_MAX;
		h[4] = 0;
		for (unsigned i = 5; i < SMP2P_ITEM_SIZE / 4; i++)
			h[i] = 0;
		dsb();
		h[1] = SMP2P_HEADER;
	}
	outbound = h;
	note("wcnss: smp2p item of ours %s at %08lx\n", kept ? "kept" : "set up", l.offset);
	kick_smp2p();
}

/* After it restarts the core flips its restart flag; the vendor answers
 * with the same value in its ack. */
static void ack_restart(void)
{
	uint32_t size = 0;
	volatile uint32_t *h = smem_get(SMEM_SMP2P_WCNSS, SMEM_HOST_WCNSS, &size);
	if (!outbound || !h || size < 20 || h[0] != SMP2P_MAGIC || !(h[1] >> 8 & SMP2P_SSR_ACK))
		return;
	bool done = h[4] & RESTART_DONE, ack = outbound[4] & RESTART_ACK;
	if (done == ack)
		return;
	outbound[4] = (outbound[4] & ~RESTART_ACK) | (done ? RESTART_ACK : 0);
	note("wcnss: smp2p restart ack %u\n", done);
	kick_smp2p();
}

/* The core's bits in its smp2p entry to us, ~0 while it has none. */
/* The header as the vendor's smp2p checks it: version 1, from the WCNSS
 * to us, no more valid entries than there are, and all of them in the
 * item. */
static const char *smp2p_why;

static uint32_t slave_bits(void)
{
	uint32_t size = 0;
	volatile uint32_t *h = smem_get(SMEM_SMP2P_WCNSS, SMEM_HOST_WCNSS, &size);
	smp2p_why = !h ? "no item" : size < 20 || h[0] != SMP2P_MAGIC ? "no header" : NULL;
	if (smp2p_why)
		return ~0u;
	unsigned total = h[3] & 0xffff, valid = h[3] >> 16;
	if ((h[1] & 0xff) != SMP2P_VERSION)
		smp2p_why = "unknown version";
	else if ((h[2] & 0xffff) != SMP2P_PID_WCNSS || h[2] >> 16 != SMP2P_PID_APPS)
		smp2p_why = "other processors";
	else if (total > SMP2P_ENTRIES_MAX || valid > total || size < 20 + total * 20)
		smp2p_why = "bad entry count";
	if (smp2p_why)
		return ~0u;
	smp2p_why = "no slave-kernel entry";
	for (unsigned i = 0; i < valid; i++) {
		volatile uint32_t *e = h + 5 + i * 5;
		if (!strncmp((const char *)(uintptr_t)e, "slave-kernel", 16)) {
			smp2p_why = NULL;
			return e[4];
		}
	}
	return ~0u;
}

static void reason(void)
{
	uint32_t size = 0;
	volatile char *s = smem_get(SMEM_PRONTO_REASON, SMEM_HOST_WCNSS, &size);
	if (!s)
		s = smem_get(SMEM_PRONTO_REASON, SMEM_HOST_ANY, &size);
	if (s && size && s[0]) {
		char buf[96];
		uint32_t n = size < sizeof buf - 1 ? size : sizeof buf - 1;
		for (uint32_t i = 0; i < n; i++)
			buf[i] = s[i];
		buf[n] = 0;
		note("wcnss: failure reason: %s\n", buf);
	}
}

/* SMEM outlives a reboot, so bits the core left there under the
 * previous system count only once they have changed since the start.
 * Without a ready bit in time, the channel is looked for anyway: that
 * wait has its own limit, and says more. Bit 2 is the vendor's proxy
 * unvote, not ready. */
/* 1 for ready, -1 for a fatal error, 0 for neither yet. */
static int ready_check(uint32_t bits, uint32_t before, bool *changed)
{
	*changed |= bits != before;
	if (bits != ~0u && (bits & SLAVE_ERR_FATAL) && *changed)
		return -1;
	return bits != ~0u && (bits & SLAVE_ERR_READY) && *changed;
}

static bool wait_ready(uint32_t before)
{
	uint32_t t0 = time_ms(), last = ~0u;
	bool changed = before == ~0u;
	const char *why = NULL;
	kick_smp2p();
	for (;;) {
		ack_restart();
		uint32_t bits = slave_bits();
		if (bits != last || smp2p_why != why) {
			note("wcnss: smp2p %08lx after %lu ms%s%s\n", bits, time_ms() - t0, smp2p_why ? ", " : "",
			     smp2p_why ? smp2p_why : "");
			last = bits;
			why = smp2p_why;
		}
		int r = ready_check(bits, before, &changed);
		if (r < 0)
			reason();
		if (r)
			return r > 0;
		if (time_ms() - t0 > READY_MS) {
			reason();
			note("wcnss: no %s after %u ms, looking for the channel anyway\n", changed ? "ready bit" : "new ready bit",
			     READY_MS);
			return true;
		}
		wait_ms(5);
	}
}

/* As the vendor kernel looks: in the primary and the secondary channel
 * table, each with its own half channel and FIFO items, as seen from
 * the WCNSS's partition. */
static const struct smd_table {
	unsigned alloc, info, fifo;
	const char *name;
} tables[] = { { 13, 14, 338, "primary" }, { 266, 138, 202, "secondary" } };

static int find_channel(const char *name, const struct smd_table **in)
{
	for (unsigned k = 0; k < ARRAY_SIZE(tables); k++) {
		uint32_t size = 0;
		volatile uint8_t *t = smem_get(tables[k].alloc, SMEM_HOST_WCNSS, &size);
		for (uint32_t i = 0; t && i < SMD_CHANNELS && (i + 1) * 32 <= size; i++) {
			volatile uint8_t *e = t + i * 32;
			uint32_t cid = *(volatile uint32_t *)(e + 20), type = *(volatile uint32_t *)(e + 24),
				 refs = *(volatile uint32_t *)(e + 28);
			if ((type & 0xff) != EDGE_APPS_WCNSS || !refs || strncmp((const char *)(uintptr_t)e, name, 20))
				continue;
			if ((type >> 8 & 0xf) != XFER_PACKET || cid >= SMD_CHANNELS) {
				note("wcnss: %s in the %s table, type %08lx cid %lu, not usable\n", name, tables[k].name, type, cid);
				return -1;
			}
			if (in)
				*in = &tables[k];
			return (int)cid;
		}
	}
	return -1;
}

static void kick(void)
{
	dsb();
	writel(KICK_SMD, APCS_IPC);
}

static bool open_channel(const char *name, struct smd_chan *c)
{
	uint32_t t0 = time_ms();
	const struct smd_table *tbl = NULL;
	int cid;
	while ((cid = find_channel(name, &tbl)) < 0) {
		if (time_ms() - t0 > CHANNEL_MS) {
			note("wcnss: no %s channel after %u ms\n", name, CHANNEL_MS);
			return false;
		}
		wait_ms(5);
	}
	uint32_t info = 0, fifo = 0;
	volatile struct smd_half *h = smem_get(tbl->info + (unsigned)cid, SMEM_HOST_WCNSS, &info);
	volatile uint8_t *f = smem_get(tbl->fifo + (unsigned)cid, SMEM_HOST_WCNSS, &fifo);
	bool fits = h && f && info == 2 * sizeof *h && fifo >= 2 * 64 && fifo % 32 == 0;
	note("wcnss: %s is channel %d of the %s table after %lu ms, info %lu fifo %lu bytes, states %lu/%lu\n", name, cid,
	     tbl->name, time_ms() - t0, info, fifo, fits ? h[0].state : 0, fits ? h[1].state : 0);
	if (!fits)
		return false;
	*c = (struct smd_chan){ &h[0], &h[1], f, f + fifo / 2, fifo / 2 };
	c->recv->tail = 0;
	c->send->head = 0;
	c->send->block_read_intr = 0;
	smd_set_state(c, SMD_OPENING);
	kick();
	t0 = time_ms();
	while (c->recv->state != SMD_OPENED) {
		if (time_ms() - t0 > OPEN_MS) {
			note("wcnss: %s not opened, remote state %lu\n", name, c->recv->state);
			smd_set_state(c, SMD_CLOSED);
			kick();
			*c = (struct smd_chan){ 0 };
			return false;
		}
		wait_ms(1);
	}
	smd_set_state(c, SMD_OPENED);
	smd_flags_seen(c);
	kick();
	note("wcnss: %s open after %lu ms\n", name, time_ms() - t0);
	return true;
}

static void close_channel(struct smd_chan *c)
{
	if (!c->send)
		return;
	smd_set_state(c, SMD_CLOSED);
	kick();
	*c = (struct smd_chan){ 0 };
}

static bool open_ctrl(void) { return open_channel("WCNSS_CTRL", &ctrl); }
static void close_ctrl(void) { close_channel(&ctrl); }

static bool ctrl_open(void)
{
	uint32_t remote = ctrl.recv->state;
	return remote == SMD_OPENED || remote == SMD_FLUSHING;
}

/* What came on the control channel since the version request. */
static struct {
	bool version, build, nv_reply, cbc;
	uint8_t major, minor;
	int nv;
	uint32_t asked, sent;
} got;
static uint8_t rx[WCNSS_MAX_MSG + 1];
static uint8_t frag[WCNSS_CAL_HEADER + WCNSS_FRAGMENT + 1];
static uint8_t cal_buf[CAL_MAX];
static struct wcnss_cal cal = { .buf = cal_buf, .max = CAL_MAX };

static bool send_now(const void *msg, uint32_t len)
{
	if (smd_packet_write(&ctrl, msg, len))
		return false;
	kick();
	return true;
}

static bool send(uint32_t type)
{
	uint8_t msg[WCNSS_HEADER];
	wcnss_msg_header(msg, type, sizeof msg);
	return send_now(msg, sizeof msg);
}

static void message(uint8_t *buf, uint32_t n)
{
	uint32_t type, len;
	if (!wcnss_msg_parse(buf, n, &type, &len)) {
		note("wcnss: message of %lu bytes, no header\n", n);
		return;
	}
	if (len != n)
		note("wcnss: message %08lx says %lu bytes, packet has %lu\n", type, len, n);
	switch (type) {
	case WCNSS_VERSION_RSP:
		if (n != 12) {
			note("wcnss: version reply of %lu bytes, not 12\n", n);
		} else if (!got.version) {
			note("wcnss: version %u.%u.%u.%u after %lu ms\n", buf[8], buf[9], buf[10], buf[11], time_ms() - got.asked);
			got.version = true;
			got.major = buf[8];
			got.minor = buf[9];
			if (!send(WCNSS_BUILD_VER_REQ))
				note("wcnss: no room for the build request\n");
		}
		break;
	case WCNSS_BUILD_VER_RSP:
		buf[n] = 0;
		note("wcnss: build %s (%lu bytes)\n", (const char *)buf + 8, n - 8);
		got.build = true;
		break;
	case WCNSS_NV_RSP:
	case WCNSS_CAL_DOWNLOAD_RSP:
		note("wcnss: %s download reply %d after %lu ms\n", type == WCNSS_NV_RSP ? "NV" : "calibration",
		     n > 8 ? buf[8] : -1, time_ms() - got.sent);
		if (type == WCNSS_NV_RSP) {
			got.nv = n > 8 ? buf[8] : -1;
			got.nv_reply = true;
		}
		break;
	case WCNSS_CBC_COMPLETE_IND:
		note("wcnss: calibration done after %lu ms\n", time_ms() - got.sent);
		got.cbc = true;
		break;
	case WCNSS_CAL_UPLOAD_REQ: {
		int r = wcnss_cal_take(&cal, buf, n);
		if (r < 0) {
			note("wcnss: calibration upload fragment not taken, data dropped\n");
		} else if (r) {
			uint8_t rsp[WCNSS_HEADER + 1];
			wcnss_msg_header(rsp, WCNSS_CAL_UPLOAD_RSP, sizeof rsp);
			rsp[WCNSS_HEADER] = WCNSS_NV_DONE;
			note("wcnss: calibration data from the core, %lu bytes%s\n", cal.have,
			     send_now(rsp, sizeof rsp) ? "" : ", no room for the reply");
		}
		break;
	}
	default:
		note("wcnss: message %08lx, %lu bytes, ignored\n", type, n);
	}
}

/* Every message waiting, or false once the channel is no use. */
static bool ctrl_poll(void)
{
	smd_flags_seen(&ctrl);
	if (!ctrl_open()) {
		note("wcnss: channel closed by the core, state %lu\n", ctrl.recv->state);
		return false;
	}
	for (;;) {
		int n = smd_packet_read(&ctrl, rx, sizeof rx - 1);
		if (n && smd_read_kick_wanted(&ctrl))
			kick();
		if (n < 0) {
			note("wcnss: bad packet on the channel\n");
			return false;
		}
		if (!n)
			return true;
		message(rx, (uint32_t)n);
	}
}

static bool wait_for(const bool *flag, uint32_t ms)
{
	uint32_t t0 = time_ms();
	while (!*flag) {
		if (!ctrl_poll() || time_ms() - t0 > ms)
			return false;
		wait_ms(1);
	}
	return true;
}

/* The version, then the build: the vendor asks for the second once the
 * first has come. Only the version counts. */
static bool versions(void)
{
	got = (typeof(got)){ .nv = -1 };
	got.asked = time_ms();
	if (!send(WCNSS_VERSION_REQ)) {
		note("wcnss: no room for the version request\n");
		return false;
	}
	if (!wait_for(&got.version, REPLY_MS)) {
		note("wcnss: no version reply\n");
		return false;
	}
	if (!wait_for(&got.build, REPLY_MS))
		note("wcnss: no build version, which is optional\n");
	return true;
}

/* The FIFO takes two fragments at a time: wait for the core to read. */
static bool send_wait(const uint8_t *msg, uint32_t len)
{
	uint32_t t0 = time_ms();
	while (smd_write_avail(&ctrl) < SMD_HEADER + len) {
		if (!ctrl_poll())
			return false;
		if (time_ms() - t0 > ROOM_MS) {
			note("wcnss: no room for %lu bytes in %u ms\n", len, ROOM_MS);
			return false;
		}
		wait_ms(1);
	}
	return send_now(msg, len);
}

/* What the core uploaded on an earlier start of this boot, before the
 * NV file, as the vendor gives it back after a restart. */
static bool cal_download(void)
{
	note("wcnss: calibration data of %lu bytes back to the core\n", cal.have);
	for (uint32_t off = 0, num = 0; off < cal.have; num++) {
		uint32_t size = cal.have - off < WCNSS_FRAGMENT ? cal.have - off : WCNSS_FRAGMENT;
		bool last = off + size == cal.have;
		memcpy(frag + WCNSS_CAL_HEADER, cal.buf + off, size);
		uint32_t len = wcnss_cal_frag(frag, cal.have, (uint16_t)num, last ? WCNSS_FRAG_LAST | WCNSS_FRAG_MORE : 0, size);
		if (!send_wait(frag, len))
			return false;
		off += size;
	}
	return true;
}

/* The NV file in fragments, read one byte past each to know the last.
 * A reply of 2 means the core calibrates first and says when done. */
static bool nv_send(void)
{
	got.sent = time_ms();
	if (cal.done && got.minor >= 5 && !cal_download())
		return false;
	uint32_t bytes = 0;
	unsigned num = 0;
	for (;; num++) {
		int n = firmware_read(NV, WCNSS_NV_SKIP + num * WCNSS_FRAGMENT, frag + WCNSS_NV_HEADER, WCNSS_FRAGMENT + 1);
		if (n < 0 || (!num && !n)) {
			note("wcnss: " NV ": %d\n", n);
			return false;
		}
		bool last = n <= WCNSS_FRAGMENT;
		uint32_t size = last ? (uint32_t)n : WCNSS_FRAGMENT;
		uint32_t len = wcnss_nv_frag(frag, (uint16_t)num, last ? WCNSS_FRAG_LAST | WCNSS_FRAG_CAL_OK : 0, size);
		if (!send_wait(frag, len)) {
			note("wcnss: NV fragment %u not sent\n", num);
			return false;
		}
		bytes += size;
		if (last)
			break;
	}
	note("wcnss: NV file, %lu bytes in %u fragments, sent in %lu ms\n", bytes, num + 1, time_ms() - got.sent);
	got.sent = time_ms();
	return true;
}

static bool nv_wanted(void)
{
	if (got.major < 1 || (got.major == 1 && got.minor < 4)) {
		note("wcnss: no NV download for version %u.%u\n", got.major, got.minor);
		return false;
	}
	return true;
}

static bool nv_download(void)
{
	if (!nv_wanted())
		return true;
	if (!nv_send())
		return false;
	if (!wait_for(&got.nv_reply, NV_MS)) {
		note("wcnss: no NV download reply\n");
		return false;
	}
	if (got.nv == WCNSS_NV_WAIT_CBC && !wait_for(&got.cbc, CBC_MS)) {
		note("wcnss: no end of calibration in %u ms\n", CBC_MS);
		return false;
	}
	return got.nv == WCNSS_NV_DONE || got.nv == WCNSS_NV_WAIT_CBC;
}

/* Bluetooth: HCI commands and events on one channel, ACL data on the
 * other, each SMD packet one HCI packet without the H4 type byte. */
#define HCI_RESET		0x0c03
#define HCI_READ_LOCAL_VERSION	0x1001
#define HCI_READ_BD_ADDR	0x1009
#define HCI_LE_READ_BUFFER_SIZE	0x2002
/* Answered by a vendor event, not Command Complete, so sent without a
 * wait; the controller takes commands in order. */
#define HCI_NVM_ACCESS		0xfc0b
#define HCI_MS			2000
#define SEND_MS			100

/* The core rings this for every write to any of its channels, and it
 * wakes the watch without an MPM pin, as the vendor kernel has it. */
#define IRQ_SMD_WCNSS		(32 + 142)

static struct smd_chan bt_cmd, bt_acl;
static uint8_t bt_rx[4 + 1024];
static uint32_t bt_dropped, smd_irqs, smd_empty;
static volatile bool smd_rang;

static void smd_irq(void)
{
	smd_irqs++;
	smd_rang = true;
}

static bool remote_open(const struct smd_chan *c)
{
	return c->send && (c->recv->state == SMD_OPENED || c->recv->state == SMD_FLUSHING);
}

static void bt_read(struct smd_chan *c, uint8_t type)
{
	if (!remote_open(c))
		return;
	smd_flags_seen(c);
	for (unsigned i = 0; i < 16; i++) {
		uint32_t avail = smd_read_avail(c);
		int n = smd_packet_read(c, bt_rx, sizeof bt_rx);
		if (n && smd_read_kick_wanted(c))
			kick();
		if (n < 0)
			bt_dropped++;
		if (n < 0 && smd_read_avail(c) == avail)
			return;
		if (n > 0)
			hci_rx_packet(type, bt_rx, (unsigned)n);
		else if (!n)
			return;
	}
}

static bool link_idle(void);

/* A ring with nothing to read is the core having read what we sent. */
static void link_poll(void)
{
	if (smd_rang) {
		smd_rang = false;
		if (link_idle())
			smd_empty++;
	}
	if (remote_open(&ctrl))
		ctrl_poll();
	bt_read(&bt_cmd, HCI_EVENT);
	bt_read(&bt_acl, HCI_ACL);
}

static int link_send(uint8_t type, const uint8_t *p, unsigned n)
{
	struct smd_chan *c = type == HCI_COMMAND ? &bt_cmd : type == HCI_ACL ? &bt_acl : NULL;
	if (!c || !remote_open(c))
		return -1;
	uint32_t t0 = time_ms();
	while (smd_packet_write(c, p, n)) {
		if (time_ms() - t0 > SEND_MS)
			return -1;
		delay_ms(1);
	}
	kick();
	return 0;
}

/* Nothing left to read; what we write is in the FIFO at once. */
static bool link_idle(void)
{
	return !(remote_open(&ctrl) && smd_read_avail(&ctrl)) && !(remote_open(&bt_cmd) && smd_read_avail(&bt_cmd)) &&
	       !(remote_open(&bt_acl) && smd_read_avail(&bt_acl));
}

static void link_wakes(uint32_t *irqs, uint32_t *empty)
{
	*irqs = smd_irqs;
	*empty = smd_empty;
}

static const struct hci_link link = { link_send, link_poll, link_idle, link_wakes };

static bool bt_open(void)
{
	if (bt_cmd.send)
		return true;
	if (core != CORE_RUNNING || !ctrl.send) {
		note("wcnss: core not running, wcnss probe keep first\n");
		return false;
	}
	if (!open_channel("APPS_RIVA_BT_CMD", &bt_cmd) || !open_channel("APPS_RIVA_BT_ACL", &bt_acl)) {
		close_channel(&bt_cmd);
		return false;
	}
	hci_set_link(&link);
	irq_set_edge(IRQ_SMD_WCNSS);
	irq_attach(IRQ_SMD_WCNSS, smd_irq);
	return true;
}

static void bt_close(void)
{
	irq_detach(IRQ_SMD_WCNSS);
	if (hci_link_attached()) {
		if (bt_host_running())
			bt_host_stop();
		hci_set_link(NULL);
	}
	close_channel(&bt_acl);
	close_channel(&bt_cmd);
}

/* Reset, then what the vendor's stack reads first. */
static bool hci(void)
{
	if (bt_host_running()) {
		note("wcnss: Bluetooth host running, wcnss stop first\n");
		return false;
	}
	if (!bt_open())
		return false;
	uint8_t rsp[32];
	unsigned n = sizeof rsp;
	uint32_t t0 = time_ms();
	int r = hci_cmd(HCI_RESET, NULL, 0, rsp, &n, HCI_MS);
	note("wcnss: HCI reset %d after %lu ms\n", r, time_ms() - t0);
	if (r)
		return false;
	uint8_t addr[6], nvm[3 + 3 + 6] = { (uint8_t)HCI_NVM_ACCESS, HCI_NVM_ACCESS >> 8, 9, 1, 2, 6 };
	if (!bt_addr_get(addr)) {
		for (unsigned i = 0; i < 6; i++)
			nvm[6 + i] = addr[5 - i];
		if (hci_send(HCI_COMMAND, nvm, sizeof nvm))
			note("wcnss: bd_addr write not sent\n");
	}
	n = sizeof rsp;
	r = hci_cmd(HCI_READ_LOCAL_VERSION, NULL, 0, rsp, &n, HCI_MS);
	if (!r && n >= 9)
		note("wcnss: hci %u rev %04x, lmp %u subversion %04x, manufacturer %u\n", rsp[1], rsp[2] | rsp[3] << 8, rsp[4],
		     rsp[7] | rsp[8] << 8, rsp[5] | rsp[6] << 8);
	else
		note("wcnss: local version %d, %u bytes\n", r, n);
	n = sizeof rsp;
	r = hci_cmd(HCI_READ_BD_ADDR, NULL, 0, rsp, &n, HCI_MS);
	if (!r && n >= 7)
		note("wcnss: bd_addr %02x:%02x:%02x:%02x:%02x:%02x\n", rsp[6], rsp[5], rsp[4], rsp[3], rsp[2], rsp[1]);
	else
		note("wcnss: bd_addr %d, %u bytes\n", r, n);
	n = sizeof rsp;
	r = hci_cmd(HCI_LE_READ_BUFFER_SIZE, NULL, 0, rsp, &n, HCI_MS);
	if (!r && n >= 4)
		note("wcnss: LE ACL %u packets of %u bytes\n", rsp[3], rsp[1] | rsp[2] << 8);
	else
		note("wcnss: LE buffer size %d, %u bytes\n", r, n);
	return true;
}

/* The NimBLE host on top, as "bt" starts it over a UART elsewhere. */
static void host(void)
{
	if (!hci())
		return;
	int r = bt_host_start();
	note(r ? "wcnss: Bluetooth host failed (%d)\n" : "wcnss: Bluetooth host starting, bt status for more\n", r);
}

static bool probe(bool keep)
{
	if (core == CORE_UNKNOWN || scm_pas_stuck()) {
		note("wcnss: core state unknown after a failed call, reboot first\n");
		return false;
	}
	if (core != CORE_OFF) {
		note("wcnss: already started, stop it first\n");
		return false;
	}
	smp2p_open();
	uint32_t size = 0, before = slave_bits();
	note("wcnss: smem channel table %s, smp2p bits %08lx before start%s%s\n",
	     smem_get(tables[0].alloc, SMEM_HOST_WCNSS, &size) ? "found" : "missing", before, smp2p_why ? ", " : "",
	     smp2p_why ? smp2p_why : "");
	bool ok = power_on() && xo_setup() && boot() && wait_ready(before) && open_ctrl() && versions() && nv_download();
	note("wcnss: probe %s\n", ok ? "done" : "stopped");
	if (!(ok && keep))
		close_ctrl();
	if (core != CORE_OFF && core != CORE_UNKNOWN && !(ok && keep))
		shutdown();
	power_off();
	return ok;
}

static void stop(void)
{
	bt_close();
	close_ctrl();
	if (core != CORE_OFF && !scm_pas_stuck())
		shutdown();
	power_off();
}

static void status(void)
{
	static const char *const names[] = { "off", "image accepted", "running", "state unknown" };
	note("wcnss: %s, smp2p bits %08lx, channel %d, %lu SMD interrupts, %lu with nothing to read\n", names[core],
	     slave_bits(), find_channel("WCNSS_CTRL", NULL), smd_irqs, smd_empty);
	if (ctrl.send) {
		note("wcnss: channel states %lu/%lu, %lu bytes waiting\n", ctrl.send->state, ctrl.recv->state, smd_read_avail(&ctrl));
		ctrl_poll();
		note("wcnss: NV reply %d, calibration %s, %lu bytes of calibration data kept\n", got.nv, got.cbc ? "done" : "not done",
		     cal.done ? cal.have : 0);
	}
	if (bt_cmd.send)
		note("wcnss: Bluetooth channel states %lu/%lu and %lu/%lu, %lu packets dropped\n", bt_cmd.send->state,
		     bt_cmd.recv->state, bt_acl.send->state, bt_acl.recv->state, bt_dropped);
	reason();
}

void wcnss_command(const char *args, void (*out)(const char *fmt, ...))
{
	say = out;
	if (chip != &chip_8009 || !board->bt.wcnss) {
		note("wcnss: not on this watch\n");
	} else if (!strncmp(args, "probe", 5)) {
		probe(!strcmp(args, "probe keep"));
	} else if (!strcmp(args, "hci")) {
		hci();
	} else if (!strcmp(args, "bt")) {
		host();
	} else if (!strcmp(args, "stop")) {
		stop();
	} else if (!strcmp(args, "status")) {
		status();
	} else {
		say("usage: wcnss probe [keep] | hci | bt | stop | status\n");
	}
	say = NULL;
}

/* The Bluetooth setting: the same steps as "wcnss probe keep" and
 * "wcnss bt", with the waits for the ready bit, the NV reply and the
 * calibration done a pass at a time, so the screen keeps going. */
static enum { START_OFF, START_READY, START_NV, START_CBC } start_step;
static uint32_t start_t, start_before;
static bool start_changed;
static struct sched_task start_task;

static void start_abort(const char *why)
{
	note("wcnss: Bluetooth start stopped at %s\n", why);
	start_step = START_OFF;
	bt_close();
	close_ctrl();
	if (core != CORE_OFF && core != CORE_UNKNOWN)
		shutdown();
	power_off();
}

static void start_done(void)
{
	start_step = START_OFF;
	note("wcnss: probe done in %lu ms\n", time_ms() - start_t);
	if (!hci()) {
		start_abort("HCI");
		return;
	}
	int r = bt_host_start();
	note(r ? "wcnss: Bluetooth host failed (%d)\n" : "wcnss: Bluetooth host starting\n", r);
}

static bool start_run(uint32_t now)
{
	if (start_step == START_READY) {
		ack_restart();
		int r = ready_check(slave_bits(), start_before, &start_changed);
		if (r < 0) {
			reason();
			start_abort("the ready bit");
			return false;
		}
		if (!r && now - start_t <= READY_MS) {
			sched_after(&start_task, now, 5);
			return false;
		}
		note("wcnss: smp2p %08lx after %lu ms\n", slave_bits(), now - start_t);
		if (!open_ctrl() || !versions()) {
			start_abort("the control channel");
			return false;
		}
		if (!nv_wanted()) {
			start_done();
			return false;
		}
		if (!nv_send()) {
			start_abort("the NV download");
			return false;
		}
		start_step = START_NV;
	}
	if (start_step == START_OFF)
		return false;
	if (!ctrl_poll()) {
		start_abort("the control channel");
		return false;
	}
	if (start_step == START_NV && got.nv_reply) {
		if (got.nv != WCNSS_NV_DONE && got.nv != WCNSS_NV_WAIT_CBC) {
			start_abort("the NV reply");
			return false;
		}
		if (got.nv == WCNSS_NV_DONE) {
			start_done();
			return false;
		}
		start_step = START_CBC;
	}
	if (start_step == START_CBC && got.cbc) {
		start_done();
		return false;
	}
	if (time_ms() - got.sent > (start_step == START_NV ? NV_MS : NV_MS + CBC_MS)) {
		start_abort(start_step == START_NV ? "the NV reply" : "the calibration");
		return false;
	}
	sched_after(&start_task, now, 10);
	return false;
}

int wcnss_bt_start(void)
{
	if (chip != &chip_8009 || !board->bt.wcnss)
		return -1;
	wcnss_bt_stop();
	if (core != CORE_OFF || scm_pas_stuck()) {
		note("wcnss: core state unknown after a failed call, reboot first\n");
		return -1;
	}
	if (!start_task.run) {
		start_task.name = "wcnss";
		start_task.run = start_run;
		sched_add(&start_task);
	}
	smp2p_open();
	start_before = slave_bits();
	start_changed = start_before == ~0u;
	start_t = time_ms();
	if (!power_on() || !xo_setup() || !boot()) {
		start_abort("the core start");
		return -1;
	}
	kick_smp2p();
	start_step = START_READY;
	sched_after(&start_task, time_ms(), 5);
	return 0;
}

void wcnss_bt_stop(void)
{
	if (start_step) {
		sched_cancel(&start_task);
		start_step = START_OFF;
	}
	if (core != CORE_OFF)
		stop();
}

bool wcnss_bt_running(void) { return start_step != START_OFF || bt_cmd.send != NULL; }
