/* The Apollo3 Blue's own BLE controller, behind its BLE interface: an SPI
 * master at 6 MHz and three side lines. WAKE (ours) asks the core to take
 * a write and STATUS (its) says it will; IRQ (its) says it has a packet,
 * which a read takes as a 2-byte length and then the packet.
 * Bring-up: the BLE buck and the feature request, power, reset and the
 * power state machine; the patches for the chip's revision as raw packets
 * the ROM acknowledges, with the chip's INFO1 trims and our address in
 * them; the length mode, the modulation trim, a check of the 32 kHz clock
 * the MCU gives the core, and the transmit power. The core runs its own
 * 32 MHz crystal and sleeps on its own between radio events once WAKE is
 * down, so the MCU may deep sleep whenever no packet waits: IRQ wakes it. */
#include <string.h>
#include "asteroid.h"
#include "sched.h"
#include "bt.h"
#include "bleif.h"
#include "bleif_proto.h"

#define BLEIF(off)		(BLEIF_BASE + (off))
#define FIFOPTR			BLEIF(0x100)
#define  FIFO0REM(v)		(((v) >> 8) & 0xffu)
#define  FIFO1SIZ(v)		(((v) >> 16) & 0xffu)
#define FIFOTHR			BLEIF(0x104)
#define FIFOPOP			BLEIF(0x108)
#define FIFOPUSH		BLEIF(0x10c)
#define FIFOCTRL		BLEIF(0x110)
#define  FIFOCTRL_POPWR		BIT(0)
#define CLKCFG			BLEIF(0x200)
#define  CLKCFG_IOCLKEN		BIT(0)
#define  CLKCFG_HFRC_DIV8	(4u << 8)
#define  CLKCFG_CLK32KEN	BIT(11)
#define CMD			BLEIF(0x20c)
#define  CMD_WRITE		1u
#define  CMD_READ		2u
#define  CMD_TSIZE(n)		((uint32_t)(n) << 8)
#define  CMD_TSIZE_MAX		0xfffu
#define OFFSETHI		BLEIF(0x214)
#define INTEN			BLEIF(0x220)
#define INTSTAT			BLEIF(0x224)
#define INTCLR			BLEIF(0x228)
#define  INT_CMDCMP		BIT(0)
#define  INT_THR		BIT(1)
#define  INT_BLECIRQ		BIT(7)
#define  INT_BLECSSTAT		BIT(8)
#define MSPICFG			BLEIF(0x300)
#define  MSPICFG_SPOL		BIT(0)
#define  MSPICFG_SPHA		BIT(1)
#define  MSPICFG_WTFCPOL	BIT(21)
#define BLECFG			BLEIF(0x304)
#define  BLECFG_PWRSMEN		BIT(0)
#define  BLECFG_WAKE		(3u << 2)
#define  BLECFG_WAKE_ON		(3u << 2)
#define  BLECFG_WAKE_OFF	(2u << 2)
#define BSTATUS			BLEIF(0x30c)
#define  BSTATUS_SPISTATUS	BIT(3)
#define  BSTATUS_BLEIRQ		BIT(7)
#define  BSTATUS_PWRST		(7u << 8)
#define  BSTATUS_PWRST_ON	(3u << 8)
#define BLEDBG			BLEIF(0x410)
#define  BLEDBG_IOCLKON		BIT(1)
#define  BLEDBG_FLAGS		7u
#define  BLEDBG_DATA(v)		((uint32_t)(v) << 3)

#define MCUCTRL_CHIPID0		(MCUCTRL_BASE + 0x004)
#define MCUCTRL_CHIPID1		(MCUCTRL_BASE + 0x008)
#define MCUCTRL_FEATUREENABLE	(MCUCTRL_BASE + 0x018)
#define  FEATURE_BLEREQ		BIT(0)
#define  FEATURE_BLE		(BIT(0) | BIT(1) | BIT(2))	/* requested, acknowledged, available */
#define MCUCTRL_MISCCTRL	(MCUCTRL_BASE + 0x198)
#define  MISCCTRL_BLE_RESETN	BIT(5)
#define MCUCTRL_BLEBUCK2	(MCUCTRL_BASE + 0x368)
#define  BLEBUCK2_TON		0xfffu
#define  BLEBUCK2_TON_TRIM	(0x19u << 6 | 0x0cu)
#define CLKGEN_BLEBUCKTONADJ	(CLKGEN_BASE + 0x3c)
#define  TONADJUSTEN		BIT(22)
#define SUPPLYSRC_BLEBUCKEN	BIT(0)
#define MISC_MEMVRLPBLE		BIT(6)
#define DEVPWRSTATUS_BLEL	BIT(8)

#define INFO1_BANDGAP		0x50023800u
#define INFO1_RETENTION		0x50023804u
#define INFO1_MODEX		0x50023808u

/* In the controller's own address space. */
#define CORE_LOCK		0x43000004u
#define CORE_TX_POWER		0x52400018u

#define HCI_RESET		0x0c03
#define HCI_READ_LOCAL_NAME	0x0c14
#define HCI_READ_LOCAL_VERSION	0x1001
#define HCI_READ_BD_ADDR	0x1009
#define VS_SET_BD_ADDR		0xfc32
#define VS_REG_READ		0xfd02
#define VS_REG_WRITE		0xfd03

#define STATUS_POLLS		300	/* of 10 us */
#define XFER_US			10000
#define START_RETRY_MS		3000
#define TX_POWER_0DBM		0x08
#define BOOT_TRIES		3
#define CLOCK_BAD		1

static bool running, patched, last_tx, task_added;
static const struct bleif_rev *rev;
static volatile uint32_t irq_count;

/* Handing packets on from here, true runs it, and with it bt_poll's
 * host work, once more before the watch may idle. */
static bool rx_run(uint32_t now) { return bleif_hci_service(); }
static struct sched_task rx_task = { .name = "bleif", .run = rx_run };

static void sleep_ms(uint32_t ms)
{
	for (uint32_t t0 = time_ms(); time_ms() - t0 < ms;)
		wdt_feed();
}

static bool wait_reg(uint32_t addr, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t t0 = time_us(); (readl(addr) & mask) != want; wdt_feed())
		if (time_us() - t0 > us)
			return false;
	return true;
}

static bool wait_irq(uint32_t ms)
{
	return wait_reg(BSTATUS, BSTATUS_BLEIRQ, BSTATUS_BLEIRQ, ms * 1000);
}

static void wake(bool on)
{
	writel((readl(BLECFG) & ~BLECFG_WAKE) | (on ? BLECFG_WAKE_ON : BLECFG_WAKE_OFF), BLECFG);
}

/* No interrupt of ours in a transfer; the IRQ edge stays latched for
 * after it. */
static uint32_t quiet(void)
{
	uint32_t en = readl(INTEN);
	writel(en & ~(INT_CMDCMP | INT_THR | INT_BLECIRQ | INT_BLECSSTAT), INTEN);
	writel(INT_CMDCMP | INT_BLECSSTAT, INTCLR);
	return en;
}

/* Before the patches STATUS is a level; after them only the edge latched
 * since the transfer began counts. */
static bool status_up(void)
{
	return patched ? readl(INTSTAT) & INT_BLECSSTAT : readl(BSTATUS) & BSTATUS_SPISTATUS;
}

static int bus_write(const uint32_t *w, unsigned n)
{
	uint32_t en = quiet();
	/* The ROM needs time between two writes. */
	if (last_tx && !patched)
		delay_ms(3);
	last_tx = false;
	if (patched)
		wait_reg(BSTATUS, BSTATUS_SPISTATUS, 0, 3000);
	writel(readl(BLEDBG) | BLEDBG_IOCLKON, BLEDBG);
	delay_us(5);
	wake(true);
	int r = -1;
	for (unsigned i = 0; i <= STATUS_POLLS; i++) {
		if (status_up()) {
			r = 0;
			break;
		}
		/* It wants to send first. */
		if (readl(BSTATUS) & BSTATUS_BLEIRQ)
			break;
		delay_us(10);
	}
	writel(readl(BLEDBG) & ~BLEDBG_IOCLKON, BLEDBG);
	if (r) {
		wake(false);
		writel(en, INTEN);
		return r;
	}
	uint32_t flags = irq_save();
	writel(0, OFFSETHI);
	/* A write takes the command word twice; the sequence known to work. */
	writel(CMD_TSIZE(n) | CMD_WRITE, CMD);
	writel(CMD_TSIZE(n) | CMD_WRITE, CMD);
	unsigned left = n;
	for (uint32_t t0 = time_us(); left;) {
		uint32_t room = FIFO0REM(readl(FIFOPTR));
		if (room < 4) {
			if ((readl(INTSTAT) & INT_CMDCMP) || time_us() - t0 > XFER_US)
				break;
			continue;
		}
		for (; room >= 4 && left; room -= 4, left = left > 4 ? left - 4 : 0)
			writel(*w++, FIFOPUSH);
	}
	wait_reg(INTSTAT, INT_CMDCMP, INT_CMDCMP, 2000);
	wake(false);
	irq_restore(flags);
	bool done = wait_reg(INTSTAT, INT_CMDCMP, INT_CMDCMP, XFER_US);
	writel(INT_CMDCMP | INT_THR, INTCLR);
	last_tx = true;
	writel(en, INTEN);
	return left || !done ? -2 : 0;
}

/* n bytes into max words; any beyond those are read and dropped. */
static int bus_read(uint32_t *w, unsigned n, unsigned max)
{
	if (!(readl(BSTATUS) & BSTATUS_BLEIRQ))
		return -1;
	uint32_t en = quiet();
	uint32_t flags = irq_save();
	writel(0, OFFSETHI);
	writel(CMD_TSIZE(n) | CMD_READ, CMD);
	unsigned got = 0;
	for (uint32_t t0 = time_us(); got < n;) {
		uint32_t have = FIFO1SIZ(readl(FIFOPTR));
		if (have < 4) {
			if (time_us() - t0 > XFER_US)
				break;
			continue;
		}
		unsigned take = have >= n ? n - got : have & ~3u;
		for (unsigned i = 0; i < take; i += 4) {
			uint32_t v = readl(FIFOPOP);
			writel(0, FIFOPOP);
			if ((got + i) / 4 < max)
				w[(got + i) / 4] = v;
		}
		got += take;
	}
	irq_restore(flags);
	bool done = wait_reg(INTSTAT, INT_CMDCMP, INT_CMDCMP, XFER_US);
	writel(INT_CMDCMP | INT_THR, INTCLR);
	if (patched)
		last_tx = false;
	writel(en, INTEN);
	return got >= n && done ? 0 : -2;
}

bool bleif_link_pending(void)
{
	return running && patched && (readl(BSTATUS) & BSTATUS_BLEIRQ);
}

int bleif_link_write(const uint32_t *w, unsigned n)
{
	return running && patched && !bus_write(w, n) ? 0 : -1;
}

/* IRQ drops a little after the read; reading again before then would
 * take the same packet twice, unless a new edge says it is a new one. */
int bleif_link_read(uint32_t *w, unsigned max)
{
	uint32_t len = 0, seen = irq_count;
	if (!bleif_link_pending())
		return 0;
	if (bus_read(&len, 2, 1))
		return -1;
	len &= 0xffff;
	if (len > CMD_TSIZE_MAX)
		return -1;
	int r = len ? bus_read(w, len, max / 4) : 0;
	for (uint32_t t0 = time_us(); (readl(BSTATUS) & BSTATUS_BLEIRQ) && irq_count == seen;)
		if (time_us() - t0 > 2000)
			break;
	return r || len > max ? -1 : (int)len;
}

static void irq(void)
{
	uint32_t s = readl(INTSTAT) & readl(INTEN);
	writel(s, INTCLR);
	irq_count++;
	if (task_added)
		sched_post(&rx_task);
}

static int patch_step(uint8_t type, uint8_t step, const uint8_t *p, unsigned n, uint32_t retry_ms)
{
	uint32_t w[(4 + BLEIF_PATCH_CHUNK) / 4], ack[2];
	unsigned len = bleif_patch_packet((uint8_t *)w, type, step, p, n);
	int r;
	for (uint32_t t0 = time_ms(); (r = bus_write(w, len)) && time_ms() - t0 < retry_ms;)
		wdt_feed();
	if (r)
		return -1;
	if (!wait_irq(1000))
		return -2;
	if (bus_read(ack, 5, 2))
		return -3;
	return bleif_patch_ack((const uint8_t *)ack, type, step) ? 0 : -4;
}

/* The first packet after power-up can take a while to be taken. */
static int send_patch(const struct bleif_patch *p, const struct bleif_edits *ed)
{
	uint8_t v[BLEIF_PATCH_CHUNK] = { (uint8_t)p->len, (uint8_t)(p->len >> 8) };
	int r = patch_step(p->type, BLEIF_PATCH_START, v, 2, START_RETRY_MS);
	unsigned off = 0;
	for (; !r && off < p->len; off += BLEIF_PATCH_CHUNK) {
		unsigned n = p->len - off < BLEIF_PATCH_CHUNK ? p->len - off : BLEIF_PATCH_CHUNK;
		bleif_patch_read(p, ed, off, n, v);
		r = patch_step(p->type, BLEIF_PATCH_DATA, v, n, 0);
	}
	if (!r) {
		uint16_t crc = bleif_patch_crc(p, ed);
		v[0] = (uint8_t)crc;
		v[1] = (uint8_t)(crc >> 8);
		r = patch_step(p->type, BLEIF_PATCH_CRC, v, 2, 0);
	}
	if (r)
		log_printf("bt: patch %02x of %u bytes failed at %u: %d\n", p->type, p->len, off, r);
	return r;
}

static int write_retry(const uint32_t *w, unsigned n)
{
	int r;
	for (uint32_t t0 = time_ms(); (r = bus_write(w, n)) && time_ms() - t0 < START_RETRY_MS;)
		wdt_feed();
	return r;
}

/* The end of patching is answered in the ROM's form, without a length;
 * then the core is told to put one in front of every packet. One that
 * already does was patched before. */
static int patch_complete(void)
{
	static const uint8_t zero[2], length_mode[] = { HCI_COMMAND, 0x04, 0xfd, 0x01, 0x01 };
	uint32_t w[3], rsp[3];
	const uint8_t *b = (const uint8_t *)rsp;
	unsigned n = bleif_patch_packet((uint8_t *)w, BLEIF_PATCH_END, BLEIF_PATCH_START, zero, 2);
	if (write_retry(w, n) || !wait_irq(100))
		return -1;
	if (bus_read(rsp, 2, 1))
		return -2;
	if (b[1] != BLEIF_PATCH_END) {
		patched = true;
		return bus_read(rsp, b[0] | b[1] << 8, 3);
	}
	if (bus_read(rsp, 3, 1))
		return -3;
	memcpy(w, length_mode, sizeof length_mode);
	if (write_retry(w, sizeof length_mode) || !wait_irq(100))
		return -4;
	if (bus_read(rsp, 9, 3))
		return -5;
	wake(false);
	patched = true;
	/* For the core to take the patches. */
	sleep_ms(500);
	return 0;
}

static int reg_read(uint32_t addr, uint32_t *v)
{
	uint8_t p[4] = { (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24) }, rsp[5];
	unsigned n = sizeof rsp;
	int r = hci_cmd(VS_REG_READ, p, sizeof p, rsp, &n, 500);
	if (r || n < sizeof rsp)
		return r ? r : -1;
	*v = rsp[1] | rsp[2] << 8 | rsp[3] << 16 | (uint32_t)rsp[4] << 24;
	return 0;
}

static int reg_write(uint32_t addr, uint32_t v)
{
	uint8_t p[8];
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(addr >> 8 * i);
		p[4 + i] = (uint8_t)(v >> 8 * i);
	}
	return hci_cmd(VS_REG_WRITE, p, sizeof p, NULL, NULL, 100);
}

/* The frequency offset of the modulation, with the core's registers
 * unlocked around it. */
static int modex(uint8_t trim)
{
	uint32_t lock, v0, v8, v70;
	trim &= 0x7f;
	if (reg_read(CORE_LOCK, &lock) || reg_write(CORE_LOCK, ~0u) || reg_read(0x52000008, &v8) ||
	    reg_read(0x52000000, &v0) || reg_write(0x52000000, v0 | BIT(10)) || reg_read(0x45800070, &v70) ||
	    reg_write(0x45800070, v70 | 8) || reg_write(0x52000014, (uint32_t)trim << 1 | 1) ||
	    reg_write(0x52000008, v8 | 8) || reg_write(rev->modex_trim, trim) || reg_write(CORE_LOCK, lock))
		return -1;
	return 0;
}

static int tx_power(uint8_t level)
{
	uint32_t lock, v;
	if (reg_read(CORE_LOCK, &lock) || reg_write(CORE_LOCK, ~0u) || reg_read(rev->power_level, &v) ||
	    reg_write(CORE_TX_POWER, 0x8 | (uint32_t)(level & 0xf) << 16) ||
	    reg_write(rev->power_level, (v & ~0xffu) | level) || reg_write(CORE_LOCK, lock))
		return -1;
	return 0;
}

/* What the core measured of the 32 kHz clock, near 0x8000 when it is
 * steady. */
static int check_clock(void)
{
	uint32_t c;
	if (reg_read(rev->clock_32k, &c))
		return -1;
	if (c < 0x7b00 || c > 0x8200) {
		log_printf("bt: 32 kHz clock measured as %08lx\n", c);
		return CLOCK_BAD;
	}
	return 0;
}

static void power_off(void)
{
	if (readl(PWRCTRL_DEVPWRSTATUS) & DEVPWRSTATUS_BLEL) {
		writel(0, INTEN);
		writel(0, CLKCFG);
		writel(readl(BLEDBG) & BLEDBG_FLAGS, BLEDBG);
		writel(readl(MCUCTRL_MISCCTRL) & ~MISCCTRL_BLE_RESETN, MCUCTRL_MISCCTRL);
		writel(readl(BLECFG) & ~BLECFG_PWRSMEN, BLECFG);
	}
	periph_power(PWR_BLE, false);
	delay_us(100);
	running = patched = false;
	if (board->ble_gpio)
		gpio_set(board->ble_gpio, 1);
}

/* Stock holds the board's line low while its radio is on; it looks
 * like the antenna's switch. */
static int power_on(void)
{
	if (board->ble_gpio) {
		gpio_set(board->ble_gpio, 0);
		gpio_config(board->ble_gpio, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	}
	writel((readl(MCUCTRL_BLEBUCK2) & ~BLEBUCK2_TON) | BLEBUCK2_TON_TRIM, MCUCTRL_BLEBUCK2);
	writel(readl(CLKGEN_BLEBUCKTONADJ) & ~TONADJUSTEN, CLKGEN_BLEBUCKTONADJ);
	writel(readl(MCUCTRL_FEATUREENABLE) | FEATURE_BLEREQ, MCUCTRL_FEATUREENABLE);
	if (!wait_reg(MCUCTRL_FEATUREENABLE, FEATURE_BLE, FEATURE_BLE, 100000))
		return -1;
	writel(readl(PWRCTRL_SUPPLYSRC) | SUPPLYSRC_BLEBUCKEN, PWRCTRL_SUPPLYSRC);
	/* Lets the buck go to low power while the core sleeps. */
	writel(readl(PWRCTRL_MISC) | MISC_MEMVRLPBLE, PWRCTRL_MISC);
	periph_power(PWR_BLE, true);
	if (!wait_reg(PWRCTRL_DEVPWRSTATUS, DEVPWRSTATUS_BLEL, DEVPWRSTATUS_BLEL, 10000))
		return -2;
	running = true;
	writel(CLKCFG_CLK32KEN, CLKCFG);
	/* Part of the power-up; what this debug bit does is not documented. */
	writel((readl(BLEDBG) & BLEDBG_FLAGS) | BLEDBG_DATA(1u << 14), BLEDBG);
	writel(readl(MCUCTRL_MISCCTRL) | MISCCTRL_BLE_RESETN, MCUCTRL_MISCCTRL);
	delay_ms(5);
	writel(readl(BLECFG) | BLECFG_PWRSMEN, BLECFG);
	if (!wait_reg(BSTATUS, BSTATUS_PWRST, BSTATUS_PWRST_ON, 1000000))
		return -3;
	writel(MSPICFG_SPOL | MSPICFG_SPHA | MSPICFG_WTFCPOL, MSPICFG);
	writel(32u << 8 | 32u, FIFOTHR);
	writel(readl(FIFOCTRL) | FIFOCTRL_POPWR, FIFOCTRL);
	writel(CLKCFG_HFRC_DIV8 | CLKCFG_IOCLKEN | CLKCFG_CLK32KEN, CLKCFG);
	return 0;
}

static int boot(const struct bleif_edits *trims, const struct bleif_edits *nvds, const char **what, uint32_t *patch_ms)
{
	int r;
	patched = last_tx = false;
	*what = "power";
	if ((r = power_on()))
		return r;
	uint32_t t0 = time_ms();
	*what = "patch download";
	if (rev->copy && ((r = send_patch(rev->copy, NULL)) ||
			  (r = patch_step(BLEIF_PATCH_END, BLEIF_PATCH_START, (const uint8_t[2]){ 0 }, 2, START_RETRY_MS))))
		return r;
	for (unsigned i = 0; i < rev->count; i++) {
		const struct bleif_patch *p = rev->patches[i];
		if ((r = send_patch(p, p == rev->trimmed ? trims : p == rev->nvds ? nvds : NULL)))
			return r;
	}
	*what = "end of patching";
	if ((r = patch_complete()))
		return r;
	*patch_ms = time_ms() - t0;
	uint8_t trim = bleif_modex_trim(readl(INFO1_MODEX));
	*what = "modulation trim";
	if (trim && (r = modex(trim)))
		return r;
	*what = "32 kHz clock";
	return check_clock();
}

static void log_packet(uint8_t type, const uint8_t *p, unsigned n)
{
	if (type == HCI_EVENT)
		log_printf("bt: event %02x, %u bytes\n", p[0], n);
	else
		log_printf("bt: packet type %u, %u bytes\n", type, n);
}

static int fail(struct bt_info *info, const char *what, int r)
{
	uint32_t pkts, errs;
	hci_stats(&pkts, &errs);
	info->failed = what;
	info->status = r;
	log_printf("bt: %s failed: %d, status %08lx, interrupts %08lx, hci %lu/%lu\n", what, r,
		   running ? readl(BSTATUS) : 0, running ? readl(INTSTAT) : 0, pkts, errs);
	bt_stop();
	return r ? r : -1;
}

static int read_addr(uint8_t lsb[6])
{
	uint8_t rsp[7];
	unsigned n = sizeof rsp;
	int r = hci_cmd(HCI_READ_BD_ADDR, NULL, 0, rsp, &n, 1000);
	if (r || n < sizeof rsp)
		return r ? r : -1;
	memcpy(lsb, rsp + 1, 6);
	return 0;
}

int bt_start(const uint8_t *bdaddr, struct bt_info *info)
{
	memset(info, 0, sizeof *info);
	uint32_t t0 = time_ms(), chiprev = readl(MCUCTRL_CHIPREV);
	/* A core already on, from an earlier image, is already patched. */
	bt_stop();
	rev = bleif_rev(chiprev);
	if (!rev) {
		log_printf("bt: chip revision %08lx\n", chiprev);
		return fail(info, "patch for the chip revision", -1);
	}
	uint8_t addr[6], lsb[6], got[6];
	if (bdaddr)
		memcpy(addr, bdaddr, 6);
	else
		bleif_chip_addr(readl(MCUCTRL_CHIPID0), readl(MCUCTRL_CHIPID1), addr);
	for (int i = 0; i < 6; i++)
		lsb[i] = addr[5 - i];
	struct bleif_edits trims = { 0 }, nvds = { 0 };
	bleif_trims(rev->trimmed, readl(INFO1_BANDGAP), readl(INFO1_RETENTION), &trims);
	bleif_nvds_set(rev->nvds, NVDS_BDADDR, lsb, 6, &nvds);
	hci_reset_parser();
	hci_set_handler(log_packet);
	if (!task_added)
		task_added = sched_add(&rx_task);
	const char *what = "power";
	int r = CLOCK_BAD;
	for (unsigned i = 0; i < BOOT_TRIES && r == CLOCK_BAD; i++) {
		if (i) {
			power_off();
			sleep_ms(1000);
		}
		r = boot(&trims, &nvds, &what, &info->patch_ms);
	}
	if (r)
		return fail(info, what, r);
	if ((r = tx_power(TX_POWER_0DBM)))
		return fail(info, "transmit power", r);
	writel(INT_BLECIRQ, INTCLR);
	writel(INT_BLECIRQ, INTEN);
	irq_attach(IRQ_BLE, irq);
	if ((r = hci_cmd(HCI_RESET, NULL, 0, NULL, NULL, 1000)))
		return fail(info, "reset", r);
	if ((r = read_addr(got)))
		return fail(info, "read bd_addr", r);
	if (memcmp(got, lsb, 6)) {
		log_printf("bt: address not taken from the NVDS, setting it\n");
		if ((r = hci_cmd(VS_SET_BD_ADDR, lsb, 6, NULL, NULL, 1000)) || (r = read_addr(got)))
			return fail(info, "write bd_addr", r);
	}
	for (int i = 0; i < 6; i++)
		info->bdaddr[i] = got[5 - i];
	uint8_t rsp[249];
	unsigned n = 9;
	if ((r = hci_cmd(HCI_READ_LOCAL_VERSION, NULL, 0, rsp, &n, 1000)) || n < 9)
		return fail(info, "read local version", r);
	info->hci_version = rsp[1];
	info->hci_revision = (uint16_t)(rsp[2] | rsp[3] << 8);
	info->lmp_version = rsp[4];
	info->manufacturer = (uint16_t)(rsp[5] | rsp[6] << 8);
	info->lmp_subversion = (uint16_t)(rsp[7] | rsp[8] << 8);
	n = sizeof rsp;
	if (!hci_cmd(HCI_READ_LOCAL_NAME, NULL, 0, rsp, &n, 1000) && n > 1) {
		unsigned k = n - 1 < sizeof info->name - 1 ? n - 1 : sizeof info->name - 1;
		memcpy(info->name, rsp + 1, k);
		info->name[k] = 0;
	}
	info->total_ms = time_ms() - t0;
	uint32_t pkts, errs;
	hci_stats(&pkts, &errs);
	log_printf("bt: up in %lu ms (patch %lu ms), %s, lmp subversion %04x, hci %lu/%lu\n", info->total_ms,
		   info->patch_ms, rev->name, info->lmp_subversion, pkts, errs);
	return 0;
}

void bt_stop(void)
{
	irq_detach(IRQ_BLE);
	power_off();
	hci_reset_parser();
}

bool bt_present(void) { return true; }

bool bt_running(void)
{
	return running;
}

/* The controller's IRQ: a packet waits. */
bool bt_host_wake(void)
{
	return running && (readl(BSTATUS) & BSTATUS_BLEIRQ);
}

bool bt_link_byte(uint8_t c) { return false; }
void bt_tx_activity(void) {}
void bt_lpm_poll(void) {}
uint32_t bt_lpm_next_ms(void) { return UINT32_MAX; }

bool bt_can_sleep(void)
{
	return !running || (bleif_hci_idle() && !bt_host_wake());
}

/* The core sleeps on its own, and its IRQ wakes the MCU from deep sleep. */
void bt_sleep(bool asleep) {}
