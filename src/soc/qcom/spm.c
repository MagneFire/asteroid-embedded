/* Subsystem power managers (SAW2 v2.1). Each CPU has one that runs a
 * small command sequence when the CPU executes WFI; the L2 one controls
 * the cluster and the APC rail, and hands the cluster to the RPM so it
 * can turn off the crystal and lower the rails. The sequences are the
 * vendor's. */
#include "asteroid.h"
#include "sched.h"

#define QTMR_CNTP_CVAL_LO	(QTMR_FRAME0 + 0x20)
#define QTMR_CNTP_CVAL_HI	(QTMR_FRAME0 + 0x24)
#define QTMR_CNTP_CTL		(QTMR_FRAME0 + 0x2c)
#define IRQ_QTMR_FRAME0		(32 + 8)
#define IRQ_MPM_WAKE		(32 + 171)
#define APCS_IPC_MPM		BIT(1)
#define VMPM			(chip->rpm_msg_ram + 0x1d0)
#define MPM_PIN_TOUCH		(board->touch_pins.mpm)
#define MPM_PIN_PMIC		(board->pmic_mpm)

#define SAW_CFG			0x08
#define SAW_SPM_CTL		0x30
#define SAW_SPM_DLY		0x34
#define SAW_PMIC_DATA(n)	(0x40 + 4 * (n))
#define SAW_VERSION		0xfd0

#define SPM_CTL_EN		BIT(0)
#define SPM_CTL_ISAR		BIT(3)
#define SPM_CTL_START(off)	((uint32_t)(off) << 4)
#define SPM_CTL_PC_MODE		BIT(16)

static const uint8_t cpu_seq[] = {
	/* clock gating, offset 0 */
	0x60, 0x03, 0x60, 0x0b, 0x0f,
	/* power collapse, offset 5 */
	0x20, 0x10, 0x80, 0x30, 0x90, 0x5b, 0x60, 0x03, 0x60, 0x3b,
	0x76, 0x76, 0x0b, 0x94, 0x5b, 0x80, 0x10, 0x26, 0x30, 0x0f,
};

#define CPU_SEQ_CLOCK_GATING	0
#define CPU_SEQ_POWER_COLLAPSE	5

static const uint8_t l2_seq_8026[] = {
	/* retention, offset 0 */
	0x00, 0x03, 0x00, 0x0f,
	/* gdhs, offset 4 */
	0x00, 0x20, 0x32, 0x6b, 0xc0, 0xe0, 0xd0, 0x42, 0x03, 0x50, 0x4e,
	0x02, 0x02, 0xd0, 0xe0, 0xc0, 0x22, 0x6b, 0x02, 0x32, 0x50, 0x0f,
	/* power collapse, offset 26 */
	0x00, 0x32, 0xb0, 0x10, 0xe0, 0xd0, 0x6b, 0xc0, 0x42, 0xf0, 0x11, 0x07, 0x01, 0xb0,
	0x50, 0x4e, 0x02, 0x02, 0xc0, 0xd0, 0x12, 0xe0, 0x6b, 0x02, 0x32, 0x50, 0xf0, 0x0f,
};

/* The APQ8009W's also turns the L2 memory off, through PMIC_DATA 4 and 5. */
static const uint8_t l2_seq_8009[] = {
	0x00, 0x03, 0x00, 0x0f,
	0x00, 0x20, 0x32, 0x6b, 0xc0, 0xe0, 0xd0, 0x42, 0x03, 0x50, 0x4e,
	0x02, 0x02, 0xd0, 0xe0, 0xc0, 0x22, 0x6b, 0x02, 0x32, 0x50, 0x0f,
	0x00, 0x32, 0xb0, 0x10, 0xe0, 0xd0, 0x6b, 0xc0, 0x42, 0xf0, 0x51, 0x11, 0x07, 0x01, 0x41,
	0xb0, 0x50, 0x4e, 0x02, 0x02, 0xc0, 0xd0, 0x12, 0xe0, 0x6b, 0x02, 0x32, 0x50, 0xf0, 0x0f,
};

#define L2_SEQ_POWER_COLLAPSE	26

/* The vendor's SAW settings. The APQ8026's kernel sets only the start
 * of a mode in SPM_CTL. The APQ8009W's writes the whole register, and
 * leaves out the sleep command bit its device tree gives the power
 * collapse modes, as the RPM is told through the MPM instead. */
struct spm_chip {
	uint32_t cpu_cfg;
	uint32_t ctl_wfi, ctl_pc, ctl_l2_pc;
	const uint8_t *l2_seq;
	unsigned l2_len;
	uint32_t l2_pmic_data[6];
};

static const struct spm_chip spm_8026 = {
	.cpu_cfg = 0,
	.ctl_wfi = SPM_CTL_ISAR | SPM_CTL_START(CPU_SEQ_CLOCK_GATING) | SPM_CTL_EN,
	.ctl_pc = SPM_CTL_ISAR | SPM_CTL_START(CPU_SEQ_POWER_COLLAPSE) | SPM_CTL_EN,
	.ctl_l2_pc = SPM_CTL_START(L2_SEQ_POWER_COLLAPSE) | SPM_CTL_EN,
	.l2_seq = l2_seq_8026,
	.l2_len = sizeof l2_seq_8026,
	.l2_pmic_data = { 0x02030080, 0x00030000 },
};

static const struct spm_chip spm_8009 = {
	.cpu_cfg = 0x01,
	.ctl_wfi = SPM_CTL_START(CPU_SEQ_CLOCK_GATING) | SPM_CTL_EN,
	.ctl_pc = SPM_CTL_PC_MODE | SPM_CTL_START(CPU_SEQ_POWER_COLLAPSE) | SPM_CTL_EN,
	.ctl_l2_pc = SPM_CTL_PC_MODE | SPM_CTL_START(L2_SEQ_POWER_COLLAPSE) | SPM_CTL_EN,
	.l2_seq = l2_seq_8009,
	.l2_len = sizeof l2_seq_8009,
	.l2_pmic_data = { 0x05030080, 0x00030000, 0, 0, 0x00010080, 0x00010000 },
};

static const struct spm_chip *spm;
static uint32_t seq_base;

static bool ready, can_collapse, cluster_parked;
static uint32_t collapses, cluster_collapses, bailouts;

#define ACC_PWR_STATUS		0x08
#define ACC_COLLAPSED		BIT(19)
#define CPUS_123		(BIT(1) | BIT(2) | BIT(3))

int cpu_suspend(uint32_t flag);
void cpu_resume(void);
void secondary_park(void);
extern volatile uint32_t park_count[16];

static void load_seq(uint32_t saw, const uint8_t *seq, unsigned len)
{
	for (unsigned w = 0; w < 32; w++) {
		uint32_t v = 0;
		for (unsigned b = 0; b < 4; b++)
			if (w * 4 + b < len)
				v |= (uint32_t)seq[w * 4 + b] << (b * 8);
		writel(v, saw + seq_base + w * 4);
	}
}

void spm_cpu_mode(enum spm_mode mode)
{
	if (!ready)
		return;
	writel(mode == SPM_POWER_COLLAPSE ? spm->ctl_pc : spm->ctl_wfi, SAW_CPU0 + SAW_SPM_CTL);
	dsb();
}

static void qtmr_irq(void)
{
	writel(0, QTMR_CNTP_CTL);
}

static void mpm_wake_irq(void)
{
}

/* The MPM watches the wake-up sources while the cluster is off. Its
 * register image in RPM memory is copied into the hardware when the RPM
 * is interrupted; the wake-up time is in system counter ticks. */
static uint32_t extra_pins[2], extra_high[2];

void wake_pin(unsigned pin, bool on, bool high)
{
	if (on)
		extra_pins[pin / 32] |= BIT(pin % 32);
	else
		extra_pins[pin / 32] &= ~BIT(pin % 32);
	if (high)
		extra_high[pin / 32] |= BIT(pin % 32);
	else
		extra_high[pin / 32] &= ~BIT(pin % 32);
}

static void mpm_arm(uint64_t wake)
{
	volatile uint32_t *v = (volatile uint32_t *)(uintptr_t)VMPM;
	uint32_t enable[2] = { BIT(MPM_PIN_TOUCH) | extra_pins[0], BIT(MPM_PIN_PMIC - 32) | extra_pins[1] };
	uint32_t polarity[2] = { extra_high[0], BIT(MPM_PIN_PMIC - 32) | extra_high[1] };
	for (unsigned i = 0; i < 2; i++) {
		v[0 + i] = (uint32_t)(wake >> (32 * i));
		v[2 + i] = enable[i];
		v[4 + i] = 0;
		v[6 + i] = 0;
		v[8 + i] = polarity[i];
		v[10 + i] = 0;
	}
	dsb();
	writel(APCS_IPC_MPM, APCS_IPC);
}

/* Idle with the core, or the whole cluster, powered down until the next
 * tick or interrupt. The core's own timer goes down with it, so the
 * wake-up comes from the memory-mapped timer, or from the MPM when the
 * cluster is off. Turning the cluster off and on takes several ms, so
 * it is only done for longer sleeps. */
void cpu_idle(bool cluster)
{
	/* With interrupts masked, an event posted before this point is seen
	 * here and one posted after it ends the wait: wfi returns for a
	 * masked interrupt, and it is taken on unmasking. */
	uint32_t ppis;
	__asm__ volatile("cpsid i" ::: "memory");
	if (!can_collapse) {
		if (!sched_pending())
			wfi();
		__asm__ volatile("cpsie i" ::: "memory");
		return;
	}
	uint64_t deadline = timer_deadline();
	int64_t left = (int64_t)(deadline - timer_count());
	if (left <= 0 || sched_pending()) {
		__asm__ volatile("cpsie i" ::: "memory");
		return;
	}
	cluster = cluster && cluster_parked && left >= timer_hz() / 50;
	if (cluster) {
		mpm_arm(deadline);
		writel(spm->ctl_l2_pc, SAW_L2 + SAW_SPM_CTL);
	}
	/* Off while the halves change, or one half-written may be past. */
	writel(0, QTMR_CNTP_CTL);
	writel((uint32_t)deadline, QTMR_CNTP_CVAL_LO);
	writel((uint32_t)(deadline >> 32), QTMR_CNTP_CVAL_HI);
	writel(1, QTMR_CNTP_CTL);
	ppis = gic_cpu_save();
	spm_cpu_mode(SPM_POWER_COLLAPSE);
	if (cpu_suspend(cluster) == 0) {
		if (cluster)
			cluster_collapses++;
		else
			collapses++;
		gic_cpu_restore(ppis);
		timer_resume();
	} else {
		bailouts++;
	}
	spm_cpu_mode(SPM_CLOCK_GATING);
	if (cluster)
		writel(0, SAW_L2 + SAW_SPM_CTL);
	__asm__ volatile("cpsie i" ::: "memory");
}

void cpu_idle_stats(uint32_t *c, uint32_t *cc, uint32_t *b)
{
	*c = collapses;
	*cc = cluster_collapses;
	*b = bailouts;
}

/* Power-up sequence of a secondary core's ACC, from reset into the
 * secure world's cold boot path. */
static void release_secondary(unsigned cpu)
{
	uint32_t acc = APCS_ACC(cpu);
	writel(0x00000033, acc + 0x04);
	writel(0x10000001, acc + 0x14);
	dsb();
	delay_us(2);
	writel(0x00000031, acc + 0x04);
	writel(0x00000039, acc + 0x04);
	dsb();
	delay_us(2);
	writel(0x00020038, acc + 0x04);
	dsb();
	delay_us(2);
	writel(0x00020008, acc + 0x04);
	writel(0x00020088, acc + 0x04);
	dsb();
}

static bool park_secondaries(void)
{
	uint32_t park = (uint32_t)(uintptr_t)secondary_park + phys_offset;
	int c = scm_set_boot_addr(park, CPUS_123, false);
	int w = scm_set_boot_addr(park, CPUS_123, true);
	if (c || w) {
		log_printf("spm: secondary boot address not accepted (%d %d)\n", c, w);
		return false;
	}
	for (unsigned cpu = 1; cpu < 4; cpu++) {
		uint32_t saw = SAW_CPU0 + cpu * 0x10000;
		load_seq(saw, cpu_seq, sizeof cpu_seq);
		writel(spm->cpu_cfg, saw + SAW_CFG);
		writel(0x3c102800, saw + SAW_SPM_DLY);
		writel(spm->ctl_wfi & ~SPM_CTL_EN, saw + SAW_SPM_CTL);
	}
	dcache_clean_invalidate_range((const void *)park_count, sizeof park_count);
	bool ok = true;
	for (unsigned cpu = 1; cpu < 4; cpu++) {
		release_secondary(cpu);
		uint32_t t0 = time_us(), st;
		do {
			dcache_invalidate_range((const void *)park_count, sizeof park_count);
			st = readl(APCS_ACC(cpu) + ACC_PWR_STATUS);
		} while ((!park_count[cpu] || !(st & ACC_COLLAPSED)) && time_us() - t0 < 100000);
		if (!park_count[cpu] || !(st & ACC_COLLAPSED)) {
			log_printf("spm: cpu%u did not park, status %08lx\n", cpu, st);
			ok = false;
		}
	}
	return ok;
}

bool cpu_idle_init(void)
{
	/* The APQ8009W resumes from a collapse, and starts the other cores,
	 * with the MMU off at the physical address, which this code is not
	 * written for yet: it only waits for interrupts. */
	if (chip == &chip_8009) {
		log_printf("spm: power collapse not used on %s\n", chip->name);
		return false;
	}
	spm = chip == &chip_8009 ? &spm_8009 : &spm_8026;
	/* The sequence memory moved in versions 2.3 and 3.0, which read
	 * as 3.0 and 1.0. */
	uint32_t ver = readl(SAW_CPU0 + SAW_VERSION), major = ver >> 28, minor = (ver >> 16) & 0xfff;
	if (major == 2 && minor == 1) {
		seq_base = 0x80;
	} else if ((major == 3 || major == 1) && minor == 0) {
		seq_base = 0x400;
	} else {
		log_printf("spm: unexpected SAW version %08lx\n", ver);
		return false;
	}
	log_printf("spm: SAW version %lu.%lu\n", major, minor);
	load_seq(SAW_CPU0, cpu_seq, sizeof cpu_seq);
	writel(spm->cpu_cfg, SAW_CPU0 + SAW_CFG);
	writel(0x3c102800, SAW_CPU0 + SAW_SPM_DLY);
	writel(0, SAW_L2 + SAW_SPM_CTL);
	load_seq(SAW_L2, spm->l2_seq, spm->l2_len);
	writel(0x14, SAW_L2 + SAW_CFG);
	writel(0x3c102800, SAW_L2 + SAW_SPM_DLY);
	for (unsigned i = 0; i < ARRAY_SIZE(spm->l2_pmic_data); i++)
		if (spm->l2_pmic_data[i])
			writel(spm->l2_pmic_data[i], SAW_L2 + SAW_PMIC_DATA(i));
	ready = true;
	spm_cpu_mode(SPM_CLOCK_GATING);
	int ret = scm_set_boot_addr((uint32_t)(uintptr_t)cpu_resume + phys_offset, BIT(0), true);
	if (ret)
		log_printf("spm: warm boot address not accepted (%d)\n", ret);
	can_collapse = !ret;
	cluster_parked = can_collapse && park_secondaries();
	writel(0, QTMR_CNTP_CTL);
	irq_attach(IRQ_QTMR_FRAME0, qtmr_irq);
	irq_set_edge(IRQ_MPM_WAKE);
	irq_attach(IRQ_MPM_WAKE, mpm_wake_irq);
	return true;
}
