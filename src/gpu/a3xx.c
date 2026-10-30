/* Adreno 305B. The GPU sits behind its own power domain (OXILICX GDSC in
 * the multimedia clock controller) and three clock branches; its core
 * and OCMEM clocks are RPM resources. Once powered it is configured as
 * the Linux driver does for this revision (VBIF limits, RBBM, CP
 * protection), the command processor's two microcode images are loaded
 * from the boot image's ramdisk (PM4 for the micro engine, PFP for the
 * prefetch parser, the first word of each file being a version), the micro engine is released and
 * initialised with CP_ME_INIT, and commands flow through a ring buffer
 * in uncached DRAM: type-0 packets write registers, type-3 packets carry
 * an opcode. Commands are submitted without waiting; new ones only
 * overwrite words the command processor has read. Its ring interrupt
 * wakes the CPU out of WFI while it waits for the GPU. */
#include <string.h>
#include "asteroid.h"
#include "a3xx.h"
#include "a3xx_ring.h"

#define GPU_BASE		0xfdb00000u
#define IRQ_GPU			(32 + 33)
#define MMSS_OXILI_GFX3D_CBCR	(MMSS_CC_BASE + 0x4028)
#define MMSS_OXILICX_BCR	(MMSS_CC_BASE + 0x4030)
#define MMSS_OXILICX_GDSCR	(MMSS_CC_BASE + 0x4034)
#define MMSS_OXILICX_AXI_CBCR	(MMSS_CC_BASE + 0x4038)
#define MMSS_OXILICX_AHB_CBCR	(MMSS_CC_BASE + 0x403c)
#define MMSS_MMSSNOC_AXI_CBCR	(MMSS_CC_BASE + 0x506c)
#define GDSC_SW_COLLAPSE	BIT(0)
#define GDSC_EN_FEW_WAIT_MASK	(0xf << 16)
#define GDSC_EN_FEW_WAIT_256	(0x2 << 16)
#define GDSC_PWR_ON		BIT(31)
#define CBCR_ENABLE		BIT(0)
#define CBCR_OFF		BIT(31)

#define RPM_CLK2		0x326b6c63u
#define RPM_KEY_SWEN		0x6e657773u
#define RPM_KEY_KHZ		0x007a484bu
#define RPM_CLK_GFX3D		1

#define REG_RBBM_HW_VERSION		0x0000
#define REG_RBBM_CLOCK_CTL		0x0010
#define REG_RBBM_SP_HYST_CNT		0x0012
#define REG_RBBM_SW_RESET_CMD		0x0018
#define REG_RBBM_AHB_CTL0		0x0020
#define REG_RBBM_AHB_CTL1		0x0021
#define REG_RBBM_STATUS			0x0030
#define REG_RBBM_WAIT_IDLE_CLOCKS_CTL	0x0033
#define REG_RBBM_INTERFACE_HANG_INT_CTL	0x0050
#define REG_RBBM_INT_CLEAR_CMD		0x0061
#define REG_RBBM_INT_0_MASK		0x0063
#define REG_RBBM_INT_0_STATUS		0x0064
#define REG_RBBM_PERFCTR_CTL		0x0080
#define REG_RBBM_GPU_BUSY_MASKED	0x0088
#define REG_RBBM_RBBM_CTL		0x0100
#define REG_CP_RB_BASE			0x01c0
#define REG_CP_RB_CNTL			0x01c1
#define REG_CP_RB_RPTR			0x01c4
#define REG_CP_RB_WPTR			0x01c5
#define REG_CP_PFP_UCODE_ADDR		0x01c9
#define REG_CP_PFP_UCODE_DATA		0x01ca
#define REG_CP_QUEUE_THRESHOLDS		0x01d5
#define REG_CP_ME_CNTL			0x01f6
#define REG_CP_ME_STATUS		0x01f7
#define REG_CP_ME_RAM_WADDR		0x01f8
#define REG_CP_ME_RAM_DATA		0x01fa
#define REG_CP_DEBUG			0x01fc
#define REG_CP_HW_FAULT			0x045c
#define REG_CP_PROTECT_CTRL		0x045e
#define REG_CP_PROTECT_STATUS		0x045f
#define REG_CP_PROTECT(n)		(0x0460 + (n))
#define REG_CP_STAT			0x047f
#define REG_CP_SCRATCH_REG(n)		(0x0578 + (n))
#define REG_UCHE_CACHE_MODE_CONTROL	0x0e82
#define REG_VBIF_IN_RD_LIM_CONF0	0x302c
#define REG_VBIF_IN_WR_LIM_CONF0	0x3030
#define REG_VBIF_OUT_RD_LIM_CONF0	0x3034
#define REG_VBIF_OUT_WR_LIM_CONF0	0x3035
#define REG_VBIF_DDR_OUT_MAX_BURST	0x3036
#define REG_VBIF_ROUND_ROBIN_QOS_ARB	0x3049

#define RBBM_STATUS_GPU_BUSY		BIT(31)
#define CP_RB_CNTL_NO_UPDATE		BIT(27)
#define CP_DEBUG_MIU_128BIT_WRITE	BIT(25)
#define CP_DEBUG_DYNAMIC_CLK_DISABLE	BIT(27)
#define CP_ME_CNTL_HALT			BIT(28)
#define RBBM_INT_CP_RB			BIT(15)

#define RING_WORDS		(32768 / 4)


static volatile uint32_t *const ring = (volatile uint32_t *)(uintptr_t)GPU_MEM_BASE;
static volatile uint32_t *const scratch = (volatile uint32_t *)(uintptr_t)(GPU_MEM_BASE + 0x10000);
static uint32_t wptr, submitted;
static bool ready;

static void gpu_write(uint32_t reg, uint32_t v) { writel(v, GPU_BASE + reg * 4); }
static uint32_t gpu_read(uint32_t reg) { return readl(GPU_BASE + reg * 4); }

static void emit(uint32_t v)
{
	ring[wptr] = v;
	wptr = (wptr + 1) % RING_WORDS;
}

static void submit(void)
{
	dsb();
	gpu_write(REG_CP_RB_WPTR, wptr);
	submitted = wptr;
}

static bool flush(uint32_t timeout_ms)
{
	submit();
	uint32_t t0 = time_ms();
	while (gpu_read(REG_CP_RB_RPTR) != wptr || (gpu_read(REG_RBBM_STATUS) & RBBM_STATUS_GPU_BUSY)) {
		if (time_ms() - t0 > timeout_ms) {
			log_printf("gpu: stuck, rptr %lu wptr %lu status %08lx cp_stat %08lx me %08lx fault %08lx prot %08lx int %08lx\n",
				   gpu_read(REG_CP_RB_RPTR), wptr, gpu_read(REG_RBBM_STATUS), gpu_read(REG_CP_STAT),
				   gpu_read(REG_CP_ME_STATUS), gpu_read(REG_CP_HW_FAULT), gpu_read(REG_CP_PROTECT_STATUS),
				   gpu_read(REG_RBBM_INT_0_STATUS));
			return false;
		}
	}
	return true;
}

void gpu_ring_emit(uint32_t v)
{
	emit(v);
}

void gpu_ring_reserve(uint32_t n)
{
	uint32_t rptr = gpu_read(REG_CP_RB_RPTR);
	if ((rptr + RING_WORDS - wptr - 1) % RING_WORDS < n + 16)
		gpu_ring_flush(1000);
}

void gpu_ring_submit(void)
{
	if (ready)
		submit();
}

static void gpu_irq(void)
{
	gpu_write(REG_RBBM_INT_CLEAR_CMD, gpu_read(REG_RBBM_INT_0_STATUS));
}

bool gpu_ready(void)
{
	return ready;
}

bool gpu_ring_flush(uint32_t timeout_ms)
{
	return ready && flush(timeout_ms);
}

static bool branch_on(uint32_t cbcr)
{
	writel(readl(cbcr) | CBCR_ENABLE, cbcr);
	for (int i = 0; i < 1000; i++) {
		if (!(readl(cbcr) & CBCR_OFF))
			return true;
		delay_us(1);
	}
	log_printf("gpu: clock %08lx stays off (%08lx)\n", cbcr, readl(cbcr));
	return false;
}

static bool power_on(void)
{
	uint32_t kvp[6] = { RPM_KEY_SWEN, 4, 1, RPM_KEY_KHZ, 4, 200000 };
	rpm_request(RPM_CLK2, RPM_CLK_GFX3D, kvp, 2);
	uint32_t g = readl(MMSS_OXILICX_GDSCR);
	if (!(g & GDSC_PWR_ON)) {
		g &= ~(GDSC_SW_COLLAPSE | GDSC_EN_FEW_WAIT_MASK);
		writel(g | GDSC_EN_FEW_WAIT_256, MMSS_OXILICX_GDSCR);
		uint32_t t0 = time_ms();
		while (!(readl(MMSS_OXILICX_GDSCR) & GDSC_PWR_ON))
			if (time_ms() - t0 > 100) {
				log_printf("gpu: power domain stays off (%08lx)\n", readl(MMSS_OXILICX_GDSCR));
				return false;
			}
	}
	return branch_on(MMSS_MMSSNOC_AXI_CBCR) && branch_on(MMSS_OXILICX_AHB_CBCR) &&
	       branch_on(MMSS_OXILICX_AXI_CBCR) && branch_on(MMSS_OXILI_GFX3D_CBCR);
}

static void load_ucode(uint32_t addr_reg, uint32_t data_reg, const uint32_t *fw, const uint32_t *end)
{
	gpu_write(addr_reg, 0);
	for (const uint32_t *p = fw + 1; p < end; p++)
		gpu_write(data_reg, *p);
}

bool gpu_init(void)
{
	uint32_t pm4_len, pfp_len;
	const uint32_t *pm4 = boot_blob("a330_pm4.fw", &pm4_len), *pfp = boot_blob("a330_pfp.fw", &pfp_len);
	if (!pm4 || !pfp || pm4_len < 8 || pfp_len < 24) {
		log_printf("gpu: no microcode in the boot image, not started\n");
		return false;
	}
	if (!power_on())
		return false;
	uint32_t ver = gpu_read(REG_RBBM_HW_VERSION);
	log_printf("gpu: powered, hw version %08lx, gdsc %08lx\n", ver, readl(MMSS_OXILICX_GDSCR));

	gpu_write(REG_VBIF_IN_RD_LIM_CONF0, 0x00181818);
	gpu_write(REG_VBIF_IN_WR_LIM_CONF0, 0x00181818);
	gpu_write(REG_VBIF_OUT_RD_LIM_CONF0, 0x00000018);
	gpu_write(REG_VBIF_OUT_WR_LIM_CONF0, 0x00000018);
	gpu_write(REG_VBIF_DDR_OUT_MAX_BURST, 0x00000303);
	gpu_write(REG_VBIF_ROUND_ROBIN_QOS_ARB, 0x0003);
	gpu_write(REG_RBBM_GPU_BUSY_MASKED, 0xffffffff);
	gpu_write(REG_RBBM_SP_HYST_CNT, 0x10);
	gpu_write(REG_RBBM_WAIT_IDLE_CLOCKS_CTL, 0x10);
	gpu_write(REG_RBBM_AHB_CTL0, 0x00000001);
	gpu_write(REG_RBBM_AHB_CTL1, 0xa6ffffff);
	gpu_write(REG_RBBM_RBBM_CTL, 0x00030000);
	gpu_write(REG_RBBM_INTERFACE_HANG_INT_CTL, 0x00010fff);
	gpu_write(REG_UCHE_CACHE_MODE_CONTROL, 0x00000001);
	gpu_write(REG_RBBM_CLOCK_CTL, 0xaaaaaaaa);
	gpu_write(REG_RBBM_PERFCTR_CTL, 0x01);
	gpu_write(REG_RBBM_INT_CLEAR_CMD, 0xffffffff);
	gpu_write(REG_RBBM_INT_0_MASK, RBBM_INT_CP_RB);
	gic_enable(IRQ_GPU, gpu_irq);

	memset((void *)ring, 0, RING_WORDS * 4);
	wptr = submitted = 0;
	gpu_write(REG_CP_RB_CNTL, 12 | 2 << 8 | CP_RB_CNTL_NO_UPDATE);
	gpu_write(REG_CP_RB_BASE, (uint32_t)(uintptr_t)ring);
	gpu_write(REG_CP_PROTECT_CTRL, 0x00000007);
	static const uint32_t protect[] = {
		0x63000040, 0x62000080, 0x600000cc, 0x60000108, 0x64000140, 0x66000400, 0x65000700,
		0x610007d8, 0x620007e0, 0x61001178, 0x64001180, 0x60003300, 0x6b00c000,
	};
	for (unsigned i = 0; i < ARRAY_SIZE(protect); i++)
		gpu_write(REG_CP_PROTECT(i), protect[i]);

	gpu_write(REG_CP_DEBUG, CP_DEBUG_DYNAMIC_CLK_DISABLE | CP_DEBUG_MIU_128BIT_WRITE);
	load_ucode(REG_CP_ME_RAM_WADDR, REG_CP_ME_RAM_DATA, pm4, pm4 + pm4_len / 4);
	load_ucode(REG_CP_PFP_UCODE_ADDR, REG_CP_PFP_UCODE_DATA, pfp, pfp + pfp_len / 4);
	gpu_write(REG_CP_QUEUE_THRESHOLDS, 0x003e2008);
	gpu_write(REG_CP_ME_CNTL, 0);

	static const uint32_t me_init[] = {
		0x000003f7, 0, 0, 0, 0x00000080, 0x00000100, 0x00000180, 0x00006600,
		0x00000150, 0x0000014e, 0x00000154, 0x00000001, 0, 0, 0x20000000, 0, 0,
	};
	emit(CP_TYPE3(CP_ME_INIT, ARRAY_SIZE(me_init)));
	for (unsigned i = 0; i < ARRAY_SIZE(me_init); i++)
		emit(me_init[i]);
	ready = flush(100);
	log_printf("gpu: pm4 v%08lx pfp v%08lx, micro engine %s\n", pm4[1], pfp[5],
		   ready ? "running" : "did not start");
	return ready;
}

bool gpu_selftest(void)
{
	if (!ready)
		return false;
	scratch[0] = 0;
	uint32_t t0 = time_us();
	emit(CP_TYPE0(REG_CP_SCRATCH_REG(2), 1));
	emit(0x5eed0001);
	emit(CP_TYPE3(CP_MEM_WRITE, 2));
	emit((uint32_t)(uintptr_t)scratch);
	emit(0xc0ffee42);
	emit(CP_TYPE3(CP_WAIT_FOR_IDLE, 1));
	emit(0);
	bool ok = flush(100);
	uint32_t us = time_us() - t0;
	uint32_t reg = gpu_read(REG_CP_SCRATCH_REG(2)), mem = scratch[0];
	ok = ok && reg == 0x5eed0001 && mem == 0xc0ffee42;
	log_printf("gpu: selftest %s in %lu us: scratch reg %08lx, memory %08lx\n", ok ? "passed" : "FAILED", us, reg, mem);
	return ok;
}

/* Power the GPU down while it is not needed: halt the micro engine,
 * gate its clocks, collapse its power domain and drop the core clock
 * vote. gpu_init() brings it back. */
void gpu_suspend(void)
{
	if (!ready)
		return;
	uint32_t t0 = time_ms();
	while ((gpu_read(REG_RBBM_STATUS) & RBBM_STATUS_GPU_BUSY) && time_ms() - t0 < 50)
		;
	gpu_write(REG_RBBM_INT_0_MASK, 0);
	gpu_write(REG_RBBM_INT_CLEAR_CMD, 0xffffffff);
	gic_mask(IRQ_GPU);
	gpu_write(REG_CP_ME_CNTL, CP_ME_CNTL_HALT);
	ready = false;
	writel(readl(MMSS_OXILI_GFX3D_CBCR) & ~CBCR_ENABLE, MMSS_OXILI_GFX3D_CBCR);
	writel(readl(MMSS_OXILICX_AXI_CBCR) & ~CBCR_ENABLE, MMSS_OXILICX_AXI_CBCR);
	writel(readl(MMSS_OXILICX_AHB_CBCR) & ~CBCR_ENABLE, MMSS_OXILICX_AHB_CBCR);
	writel(readl(MMSS_OXILICX_GDSCR) | GDSC_SW_COLLAPSE, MMSS_OXILICX_GDSCR);
	uint32_t kvp[3] = { RPM_KEY_SWEN, 4, 0 };
	rpm_request(RPM_CLK2, RPM_CLK_GFX3D, kvp, 1);
}

