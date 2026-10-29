/* Subsystem power managers (SAW2 v2.1). Each CPU has one that runs a
 * small command sequence when the CPU executes WFI; the L2 one controls
 * the cluster and the APC rail, and hands the cluster to the RPM so it
 * can turn off the crystal and lower the rails. The sequences are the
 * vendor's. */
#include "asteroid.h"

#define SAW_CPU0		0xf9089000u
#define SAW_L2			0xf9012000u
#define QTMR_FRAME0		0xf9021000u
#define QTMR_CNTP_TVAL		(QTMR_FRAME0 + 0x28)
#define QTMR_CNTP_CTL		(QTMR_FRAME0 + 0x2c)
#define IRQ_QTMR_FRAME0		(32 + 8)
#define IRQ_MPM_WAKE		(32 + 171)
#define APCS_IPC		0xf9011008u
#define APCS_IPC_MPM		BIT(1)
#define VMPM			0xfc4281d0u
#define MPM_PIN_TOUCH		8
#define MPM_PIN_PMIC		62

#define SCM_SVC_BOOT		0x01
#define SCM_BOOT_ADDR		0x01
#define SCM_FLAG_WARMBOOT_CPU0	0x04
#define SAW_CFG			0x08
#define SAW_SPM_CTL		0x30
#define SAW_SPM_DLY		0x34
#define SAW_PMIC_DATA(n)	(0x40 + 4 * (n))
#define SAW_SEQ			0x80
#define SAW_VERSION		0xfd0

#define SPM_CTL_EN		BIT(0)
#define SPM_CTL_BASE		BIT(3)
#define SPM_CTL_START(off)	((uint32_t)(off) << 4)

static const uint8_t cpu_seq[] = {
	/* clock gating, offset 0 */
	0x60, 0x03, 0x60, 0x0b, 0x0f,
	/* power collapse, offset 5 */
	0x20, 0x10, 0x80, 0x30, 0x90, 0x5b, 0x60, 0x03, 0x60, 0x3b,
	0x76, 0x76, 0x0b, 0x94, 0x5b, 0x80, 0x10, 0x26, 0x30, 0x0f,
};

#define CPU_SEQ_CLOCK_GATING	0
#define CPU_SEQ_POWER_COLLAPSE	5

static const uint8_t l2_seq[] = {
	/* retention, offset 0 */
	0x00, 0x03, 0x00, 0x0f,
	/* gdhs, offset 4 */
	0x00, 0x20, 0x32, 0x6b, 0xc0, 0xe0, 0xd0, 0x42, 0x03, 0x50, 0x4e,
	0x02, 0x02, 0xd0, 0xe0, 0xc0, 0x22, 0x6b, 0x02, 0x32, 0x50, 0x0f,
	/* power collapse, offset 26 */
	0x00, 0x32, 0xb0, 0x10, 0xe0, 0xd0, 0x6b, 0xc0, 0x42, 0xf0, 0x11, 0x07, 0x01, 0xb0,
	0x50, 0x4e, 0x02, 0x02, 0xc0, 0xd0, 0x12, 0xe0, 0x6b, 0x02, 0x32, 0x50, 0xf0, 0x0f,
};

#define L2_SEQ_POWER_COLLAPSE	26

static bool ready, can_collapse, cluster_parked;
static uint32_t collapses, cluster_collapses, bailouts;

#define APCS_ACC(cpu)		(0xf9088000u + (cpu) * 0x10000)
#define ACC_PWR_STATUS		0x08
#define ACC_COLLAPSED		BIT(19)
#define SCM_FLAG_COLDBOOT_CPU123	(0x01 | 0x08 | 0x20)
#define SCM_FLAG_WARMBOOT_CPU123	(0x02 | 0x10 | 0x40)

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
		writel(v, saw + SAW_SEQ + w * 4);
	}
}

void spm_cpu_mode(enum spm_mode mode)
{
	if (!ready)
		return;
	uint32_t off = mode == SPM_POWER_COLLAPSE ? CPU_SEQ_POWER_COLLAPSE : CPU_SEQ_CLOCK_GATING;
	writel(SPM_CTL_BASE | SPM_CTL_START(off) | SPM_CTL_EN, SAW_CPU0 + SAW_SPM_CTL);
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
static uint32_t extra_pins[2];

void spm_wake_pin(unsigned pin, bool on)
{
	if (on)
		extra_pins[pin / 32] |= BIT(pin % 32);
	else
		extra_pins[pin / 32] &= ~BIT(pin % 32);
}

static void mpm_arm(uint64_t wake)
{
	volatile uint32_t *v = (volatile uint32_t *)(uintptr_t)VMPM;
	uint32_t enable[2] = { BIT(MPM_PIN_TOUCH) | extra_pins[0], BIT(MPM_PIN_PMIC - 32) | extra_pins[1] };
	uint32_t polarity[2] = { 0, BIT(MPM_PIN_PMIC - 32) };
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
void spm_idle(bool cluster)
{
	if (!can_collapse) {
		wfi();
		return;
	}
	uint32_t tval, ppis;
	__asm__ volatile("cpsid i" ::: "memory");
	__asm__ volatile("mrc p15, 0, %0, c14, c2, 0" : "=r"(tval));	/* CNTP_TVAL */
	if ((int32_t)tval <= 0) {
		__asm__ volatile("cpsie i" ::: "memory");
		return;
	}
	cluster = cluster && cluster_parked && tval >= timer_hz() / 50;
	if (cluster) {
		mpm_arm(timer_count() + tval);
		writel(SPM_CTL_START(L2_SEQ_POWER_COLLAPSE) | SPM_CTL_EN, SAW_L2 + SAW_SPM_CTL);
	}
	writel(tval, QTMR_CNTP_TVAL);
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

void spm_stats(uint32_t *c, uint32_t *cc, uint32_t *b)
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
	int c = scm_call2(SCM_SVC_BOOT, SCM_BOOT_ADDR, SCM_FLAG_COLDBOOT_CPU123, (uint32_t)(uintptr_t)secondary_park);
	int w = scm_call2(SCM_SVC_BOOT, SCM_BOOT_ADDR, SCM_FLAG_WARMBOOT_CPU123, (uint32_t)(uintptr_t)secondary_park);
	if (c || w) {
		log_printf("spm: secondary boot address not accepted (%d %d)\n", c, w);
		return false;
	}
	for (unsigned cpu = 1; cpu < 4; cpu++) {
		uint32_t saw = SAW_CPU0 + cpu * 0x10000;
		load_seq(saw, cpu_seq, sizeof cpu_seq);
		writel(0, saw + SAW_CFG);
		writel(0x3c102800, saw + SAW_SPM_DLY);
		writel(SPM_CTL_BASE, saw + SAW_SPM_CTL);
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

bool spm_init(void)
{
	uint32_t ver = readl(SAW_CPU0 + SAW_VERSION);
	if ((ver >> 28) != 2 || ((ver >> 16) & 0xfff) != 1) {
		log_printf("spm: unexpected SAW version %08lx\n", ver);
		return false;
	}
	load_seq(SAW_CPU0, cpu_seq, sizeof cpu_seq);
	writel(0, SAW_CPU0 + SAW_CFG);
	writel(0x3c102800, SAW_CPU0 + SAW_SPM_DLY);
	writel(0, SAW_L2 + SAW_SPM_CTL);
	load_seq(SAW_L2, l2_seq, sizeof l2_seq);
	writel(0x14, SAW_L2 + SAW_CFG);
	writel(0x3c102800, SAW_L2 + SAW_SPM_DLY);
	writel(0x02030080, SAW_L2 + SAW_PMIC_DATA(0));
	writel(0x00030000, SAW_L2 + SAW_PMIC_DATA(1));
	ready = true;
	spm_cpu_mode(SPM_CLOCK_GATING);
	int ret = scm_call2(SCM_SVC_BOOT, SCM_BOOT_ADDR, SCM_FLAG_WARMBOOT_CPU0, (uint32_t)(uintptr_t)cpu_resume);
	if (ret)
		log_printf("spm: warm boot address not accepted (%d)\n", ret);
	can_collapse = !ret;
	cluster_parked = can_collapse && park_secondaries();
	writel(0, QTMR_CNTP_CTL);
	gic_enable(IRQ_QTMR_FRAME0, qtmr_irq);
	gic_set_edge(IRQ_MPM_WAKE);
	gic_enable(IRQ_MPM_WAKE, mpm_wake_irq);
	return true;
}
