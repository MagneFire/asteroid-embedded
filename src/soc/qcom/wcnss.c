/* The APQ8009W's own WCNSS core (Pronto) with its WCN3620A RF chip, as
 * swift's vendor kernel starts it: the supplies and the RF chip's
 * crystal set up from here, then the secure world checks the signed
 * firmware from the system partition and starts the core. The core
 * says it is up through smp2p and opens its control channel over SMD;
 * the first request on it asks for its version.
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

#define IO(pa)			(IO_8009_BASE + (pa))
#define PAS_WCNSS		6
#define FW			"/etc/firmware/wcnss"
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

#define MSG_VERSION_REQ		0x01000000u
#define MSG_VERSION_RSP		0x01000001u
#define MSG_BUILD_VER_REQ	0x01000009u
#define MSG_BUILD_VER_RSP	0x0100000au

#define READY_MS		5000
#define CHANNEL_MS		3000
#define OPEN_MS			2000
#define REPLY_MS		2000

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

static void iris_reset(uint32_t reg)
{
	writel(reg | CFG_IRIS_RESET, PMU_CFG);
	wait_clear(PMU_CFG, CFG_IRIS_RESET_STS, 10000);
	writel(reg & ~CFG_IRIS_RESET, PMU_CFG);
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
	iris_reset(reg);
	writel((iris & 0xffff) | IRIS_CHIP_ID, IRIS_READ);
	bool valid = false;
	for (unsigned i = 0; i < 6 && !valid; i++) {
		writel(reg | CFG_XO_READ, PMU_CFG);
		bool read = wait_clear(PMU_CFG, CFG_XO_READ_STS, 10000);
		iris = readl(IRIS_READ);
		valid = read && iris_valid(iris);
		writel(reg, PMU_CFG);
		if (!valid)
			iris_reset(reg);
	}
	if (valid && iris >> 30 == 0)
		reg = (reg & ~CFG_XO_MODE) | CFG_XO_MODE;
	else
		reg &= ~CFG_XO_MODE;
	writel(reg, PMU_CFG);
	iris_reset(reg);
	writel(reg | CFG_XO_CFG, PMU_CFG);
	bool done = wait_clear(PMU_CFG, CFG_XO_CFG_STS, 100000);
	writel(reg & ~(CFG_BUS_MUX_TOP | CFG_XO_CFG), PMU_CFG);
	int rf = 0;
	if (!(iris >> 30 == 0 && valid)) {
		rf = rf_clock(true);
		ours.rf_clock = true;
	}
	wait_ms(20);
	note("wcnss: RF chip id %08lx (%s), crystal %s MHz, setup %s, RF clock %s\n", iris, valid ? "known" : "unknown",
	     iris >> 30 == 0 && valid ? "48" : "19.2", done ? "done" : "timed out", rf ? "not acknowledged" : "on");
	return valid && done && !rf;
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

/* The core's bits in its smp2p entry to us, ~0 while it has none. */
static uint32_t slave_bits(void)
{
	uint32_t size = 0;
	volatile uint32_t *h = smem_get(SMEM_SMP2P_WCNSS, SMEM_HOST_WCNSS, &size);
	if (!h || size < 20 || h[0] != SMP2P_MAGIC)
		return ~0u;
	unsigned valid = h[3] >> 16;
	for (unsigned i = 0; i < valid && 20 + (i + 1) * 20 <= size; i++) {
		volatile uint32_t *e = h + 5 + i * 5;
		if (!strncmp((const char *)(uintptr_t)e, "slave-kernel", 16))
			return e[4];
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

static bool wait_ready(void)
{
	uint32_t t0 = time_ms(), last = ~0u;
	for (;;) {
		uint32_t bits = slave_bits();
		if (bits != last) {
			note("wcnss: smp2p %08lx after %lu ms\n", bits, time_ms() - t0);
			last = bits;
		}
		if (bits != ~0u && (bits & SLAVE_ERR_FATAL)) {
			reason();
			return false;
		}
		if (bits != ~0u && (bits & SLAVE_ERR_READY))
			return true;
		if (time_ms() - t0 > READY_MS) {
			reason();
			return false;
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

static bool open_ctrl(void)
{
	uint32_t t0 = time_ms();
	const struct smd_table *tbl = NULL;
	int cid;
	while ((cid = find_channel("WCNSS_CTRL", &tbl)) < 0) {
		if (time_ms() - t0 > CHANNEL_MS) {
			note("wcnss: no WCNSS_CTRL channel after %u ms\n", CHANNEL_MS);
			return false;
		}
		wait_ms(5);
	}
	uint32_t info = 0, fifo = 0;
	volatile struct smd_half *h = smem_get(tbl->info + (unsigned)cid, SMEM_HOST_WCNSS, &info);
	volatile uint8_t *f = smem_get(tbl->fifo + (unsigned)cid, SMEM_HOST_WCNSS, &fifo);
	bool fits = h && f && info == 2 * sizeof *h && fifo >= 2 * 64 && fifo % 32 == 0;
	note("wcnss: WCNSS_CTRL is channel %d of the %s table after %lu ms, info %lu fifo %lu bytes, states %lu/%lu\n", cid,
	     tbl->name, time_ms() - t0, info, fifo, fits ? h[0].state : 0, fits ? h[1].state : 0);
	if (!fits)
		return false;
	ctrl = (struct smd_chan){ &h[0], &h[1], f, f + fifo / 2, fifo / 2 };
	ctrl.recv->tail = 0;
	ctrl.send->head = 0;
	ctrl.send->block_read_intr = 0;
	smd_set_state(&ctrl, SMD_OPENING);
	kick();
	t0 = time_ms();
	while (ctrl.recv->state != SMD_OPENED) {
		if (time_ms() - t0 > OPEN_MS) {
			note("wcnss: channel not opened, remote state %lu\n", ctrl.recv->state);
			return false;
		}
		wait_ms(1);
	}
	smd_set_state(&ctrl, SMD_OPENED);
	smd_flags_seen(&ctrl);
	kick();
	note("wcnss: channel open after %lu ms\n", time_ms() - t0);
	return true;
}

static void close_ctrl(void)
{
	if (!ctrl.send)
		return;
	smd_set_state(&ctrl, SMD_CLOSED);
	kick();
	ctrl = (struct smd_chan){ 0 };
}

static bool ctrl_open(void)
{
	uint32_t remote = ctrl.recv->state;
	return remote == SMD_OPENED || remote == SMD_FLUSHING;
}

static bool send(uint32_t type)
{
	const uint32_t msg[2] = { type, 8 };
	if (smd_packet_write(&ctrl, msg, sizeof msg))
		return false;
	kick();
	return true;
}

/* The version, then the build: the vendor asks for the second once the
 * first has come. */
static bool versions(void)
{
	if (!send(MSG_VERSION_REQ)) {
		note("wcnss: no room for the version request\n");
		return false;
	}
	uint32_t t0 = time_ms();
	bool version = false;
	while (time_ms() - t0 < REPLY_MS) {
		uint8_t buf[128];
		smd_flags_seen(&ctrl);
		if (!ctrl_open()) {
			note("wcnss: channel closed by the core, state %lu\n", ctrl.recv->state);
			return false;
		}
		int n = smd_packet_read(&ctrl, buf, sizeof buf - 1);
		if (n && smd_read_kick_wanted(&ctrl))
			kick();
		if (n < 0) {
			note("wcnss: bad packet on the channel\n");
			return false;
		}
		if (n < 8) {
			wait_ms(1);
			continue;
		}
		uint32_t type;
		memcpy(&type, buf, 4);
		if (type == MSG_VERSION_RSP && n == 12) {
			note("wcnss: version %u.%u.%u.%u after %lu ms\n", buf[8], buf[9], buf[10], buf[11], time_ms() - t0);
			version = true;
			send(MSG_BUILD_VER_REQ);
		} else if (type == MSG_BUILD_VER_RSP) {
			buf[n] = 0;
			note("wcnss: build %s\n", (const char *)buf + 8);
			return true;
		} else {
			note("wcnss: message %08lx, %d bytes\n", type, n);
		}
	}
	note("wcnss: %s\n", version ? "no build version" : "no version reply");
	return version;
}

static void probe(bool keep)
{
	if (core == CORE_UNKNOWN || scm_pas_stuck()) {
		note("wcnss: core state unknown after a failed call, reboot first\n");
		return;
	}
	if (core != CORE_OFF) {
		note("wcnss: already started, stop it first\n");
		return;
	}
	uint32_t size = 0;
	note("wcnss: smem channel table %s, smp2p bits %08lx before start\n",
	     smem_get(tables[0].alloc, SMEM_HOST_WCNSS, &size) ? "found" : "missing", slave_bits());
	bool ok = power_on() && xo_setup() && boot() && wait_ready() && open_ctrl() && versions();
	note("wcnss: probe %s\n", ok ? "done" : "stopped");
	if (!(ok && keep))
		close_ctrl();
	if (core != CORE_OFF && core != CORE_UNKNOWN && !(ok && keep))
		shutdown();
	power_off();
}

static void status(void)
{
	static const char *const names[] = { "off", "image accepted", "running", "state unknown" };
	note("wcnss: %s, smp2p bits %08lx, channel %d\n", names[core], slave_bits(),
	     find_channel("WCNSS_CTRL", NULL));
	if (ctrl.send)
		note("wcnss: channel states %lu/%lu, %lu bytes waiting\n", ctrl.send->state, ctrl.recv->state, smd_read_avail(&ctrl));
	reason();
}

void wcnss_command(const char *args, void (*out)(const char *fmt, ...))
{
	say = out;
	if (chip != &chip_8009 || !board->bt.wcnss) {
		note("wcnss: not on this watch\n");
	} else if (!strncmp(args, "probe", 5)) {
		probe(!strcmp(args, "probe keep"));
	} else if (!strcmp(args, "stop")) {
		close_ctrl();
		if (core != CORE_OFF && !scm_pas_stuck())
			shutdown();
		power_off();
	} else if (!strcmp(args, "status")) {
		status();
	} else {
		say("usage: wcnss probe [keep] | stop | status\n");
	}
	say = NULL;
}
