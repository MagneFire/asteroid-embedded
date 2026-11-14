/* The Qualcomm register map used by the firmware. Blocks that move
 * between the APQ8026 and the APQ8009W are in the chip table; the
 * display's MDP5, MMSS and DSI PLL registers are the APQ8026's only. */
#ifndef SOC_H
#define SOC_H
#include "memmap.h"
#ifndef __ASSEMBLER__
#include "chip.h"
#endif

#define APCS_BASE		(chip->apcs)
#define GICD_BASE		(APCS_BASE + 0x00000)
#define GICC_BASE		(APCS_BASE + 0x02000)
#define APCS_IPC		(APCS_BASE + 0x11008)
#define APCS_A7_CMD_RCGR	(APCS_BASE + 0x11050)
#define APCS_A7_CFG_RCGR	(APCS_BASE + 0x11054)
#define SAW_L2			(APCS_BASE + 0x12000)
#define A7_PLL_BASE		(APCS_BASE + 0x16000)
#define APCS_WDT_BASE		(APCS_BASE + 0x17000)
#define QTMR_FRAME0		(APCS_BASE + 0x21000)
#define APCS_ACC(cpu)		(APCS_BASE + 0x88000 + (cpu) * 0x10000)
#define SAW_CPU0		(APCS_BASE + 0x89000)
#define SLEEP_CLK_HZ		32768u

#define GCC(off)		(chip->gcc + (off))

#define IMEM_BASE		(chip->imem)
#define IMEM_DLOAD_MAGIC	(IMEM_BASE + 0x000)
#define IMEM_EDLOAD_MAGIC	(IMEM_BASE + 0xfe0)
#define IMEM_RESTART_REASON	(IMEM_BASE + 0x65c)

#define MPM2_PS_HOLD		(chip->ps_hold)

#define SPMI_ARB_VERSION	(chip->spmi_core)
#define SPMI_ARB_CHNL(n)	(chip->spmi_core + 0x800 + (n) * 0x80)
#define SPMI_INTR		(chip->spmi_intr)
#define SPMI_CNFG		(chip->spmi_cnfg)
#define IRQ_SPMI_PERIPH		(32 + 190)
#define SPMI_CMD		0x00
#define SPMI_CONFIG		0x04
#define SPMI_STATUS		0x08
#define SPMI_WDATA0		0x10
#define SPMI_WDATA1		0x14
#define SPMI_RDATA0		0x18
#define SPMI_RDATA1		0x1c
#define SPMI_RDATA2		0x20

#define MMSS_CC_BASE		0xfd8c0000u
#define MDP_GDSCR		(MMSS_CC_BASE + 0x2304)
#define MDSS_AHB_CBCR		(MMSS_CC_BASE + 0x2308)
#define MMSS_MISC_AHB_CBCR	(MMSS_CC_BASE + 0x502c)
#define MDSS_AXI_CBCR		(MMSS_CC_BASE + 0x2310)
#define MDSS_PCLK0_CBCR		(MMSS_CC_BASE + 0x2314)
#define MDSS_MDP_CBCR		(MMSS_CC_BASE + 0x231c)
#define MDSS_MDP_LUT_CBCR	(MMSS_CC_BASE + 0x2320)
#define MDSS_VSYNC_CBCR		(MMSS_CC_BASE + 0x2328)
#define MDSS_BYTE0_CBCR		(MMSS_CC_BASE + 0x233c)
#define MDSS_ESC0_CBCR		(MMSS_CC_BASE + 0x2344)
#define MDSS_BASE		0xfd900000u
#define MDP_HW_REV		(MDSS_BASE + 0x0100)
#define MDP_INTR_EN		(MDSS_BASE + 0x0110)
#define MDP_INTR_STATUS		(MDSS_BASE + 0x0114)
#define MDP_INTR_CLEAR		(MDSS_BASE + 0x0118)
#define MDP_INTR_PP0_DONE	BIT(8)
#define MDP_INTR_PP0_RD_PTR	BIT(12)
#define MDP_CTL0		(MDSS_BASE + 0x0600)
#define CTL_LAYER0		0x00
#define CTL_TOP			0x14
#define CTL_FLUSH		0x18
#define CTL_START		0x1c
#define MDP_PIPE_VIG0		(MDSS_BASE + 0x1200)
#define MDP_PIPE_RGB0		(MDSS_BASE + 0x1e00)
#define MDP_PIPE_DMA0		(MDSS_BASE + 0x2a00)
#define PIPE_SRC_SIZE		0x00
#define PIPE_SRC_IMG_SIZE	0x04
#define PIPE_SRC_XY		0x08
#define PIPE_OUT_SIZE		0x0c
#define PIPE_OUT_XY		0x10
#define PIPE_SRC0_ADDR		0x14
#define PIPE_SRC_YSTRIDE	0x24
#define PIPE_SRC_FORMAT		0x30
#define PIPE_SRC_UNPACK		0x34
#define PIPE_SRC_OP_MODE	0x38
#define PIPE_FLIP_LR		BIT(13)
#define PIPE_FLIP_UD		BIT(14)
#define CTL_FLUSH_RGB0		BIT(3)
#define CTL_FLUSH_LM0		BIT(6)
#define CTL_FLUSH_CTL		BIT(17)
#define CTL_LAYER_RGB0_STAGE1	(1 << 9)
#define MDP_INTF_SEL		(MDSS_BASE + 0x0104)
#define MDP_INTF_SEL_DSI	(1 << 8)
#define MDP_LM0			(MDSS_BASE + 0x3200)
#define LM_OUT_SIZE		0x04
#define LM_BLEND(stage)		(0x20 + (stage) * 0x30)
#define LM_BLEND_OP_FG_CONST	0x100
#define LM_BLEND_FG_ALPHA	0x04
#define MDP_PP0			(MDSS_BASE + 0x21b00)
#define PP_TEAR_CHECK_EN	0x00
#define PP_SYNC_CONFIG_VSYNC	0x04
#define PP_SYNC_CONFIG_HEIGHT	0x08
#define PP_VSYNC_INIT_VAL	0x10
#define PP_SYNC_THRESH		0x18
#define PP_START_POS		0x1c
#define PP_RD_PTR_IRQ		0x20
#define PP_AUTOREFRESH_CONFIG	0x30
#define PP_VSYNC_COUNTER_EN	BIT(19)
#define PP_VSYNC_IN_EN		BIT(20)

#define TLMM_GPIO_CFG(n)	(chip->tlmm_pins + (n) * chip->tlmm_stride)
#define TLMM_GPIO_IO(n)		(TLMM_GPIO_CFG(n) + 4)

#define MMSS_MISC_BASE		0xfd828000u
#define MMSS_DSI_CLAMP		(MMSS_MISC_BASE + 0x014)
#define MMSS_DSI_PHY_RESET	(MMSS_MISC_BASE + 0x108)
#define DSI0_BASE		(chip->dsi)
#define DSI_DMA_CMD_OFFSET	(DSI0_BASE + 0x048)
#define DSI_DMA_CMD_LENGTH	(DSI0_BASE + 0x04c)
#define DSI_CMD_MODE_DMA_CTRL	(DSI0_BASE + 0x03c)
#define DSI_CMD_MODE_DMA_SW_TRIGGER (DSI0_BASE + 0x090)
#define DSI_INT_CTRL		(DSI0_BASE + 0x110)
#define DSI_LANE_STATUS		(DSI0_BASE + 0x0a8)
#define DSI_LANE_CTRL		(DSI0_BASE + 0x0ac)
#define DSI_LANE_SWAP		(DSI0_BASE + 0x0b0)
#define DSI_DLN0_PHY_ERR	(DSI0_BASE + 0x0b4)
#define DSI_PLL_GLB_CFG		0xfd922a20u
#define DSI_PLL_LKDET_CFG2	0xfd922a64u
#define DSI_PLL_TEST_CFG	0xfd922a68u
#define DSI_PLL_STATUS		0xfd922ac0u
#define DSI_PHY_CTRL_0		0xfd922c70u
#define DSI_PHY_REGULATOR_CTRL_6 0xfd922d98u


#define USB_HS_BASE		(chip->usb)
#define USB_SBUSCFG		(USB_HS_BASE + 0x090)
#define USB_AHB_MODE		(USB_HS_BASE + 0x098)
#define USB_GENCONFIG_2		(USB_HS_BASE + 0x0a0)
#define USB_USBCMD		(USB_HS_BASE + 0x140)
#define USB_ULPI_VIEWPORT	(USB_HS_BASE + 0x170)
#define USB_PORTSC		(USB_HS_BASE + 0x184)
#define USB_PHY_CTRL		(USB_HS_BASE + 0x240)
#define USB_USBMODE		(USB_HS_BASE + 0x1a8)
#define GCC_USB_HS_BCR		GCC(chip->usb_bcr)
#define GCC_USB_HS_SYSTEM_CBCR	GCC(chip->usb_system_cbcr)
#define GCC_USB_HS_AHB_CBCR	GCC(chip->usb_ahb_cbcr)
#define GCC_USB_HS_SYSTEM_CMD_RCGR GCC(chip->usb_system_rcgr)
#define IRQ_USB_HS		(32 + 134)
#define USB_IRQ			IRQ_USB_HS

#define IRQ_PPI_TIMER_S		18
#define IRQ_PPI_TIMER		19
#define IRQ_WDT_BARK		(32 + 3)

/* The log region as the bootloader's dump command reads it: fault
 * records first, then the console ring. */
#define RAMOOPS_RECORD_SIZE	0x2000u
#define RAMOOPS_CONSOLE_SIZE	0x40000u
#define RAMOOPS_RECORD_BASE	RAMOOPS_BASE
#define RAMOOPS_CONSOLE_BASE	(RAMOOPS_BASE + (RAMOOPS_SIZE - RAMOOPS_CONSOLE_SIZE) / RAMOOPS_RECORD_SIZE * RAMOOPS_RECORD_SIZE)
/* The last record holds the boot stage, so Linux shows it too. */
#define RAMOOPS_STAGE_BASE	(RAMOOPS_CONSOLE_BASE - RAMOOPS_RECORD_SIZE)

#define GPIO_COUNT		(chip->gpios)
#define PMIC_ADDR_MAX		0xfffffu
#define PMIC_VADC		1
#define WDT_MAX_MS (0x0fffffffu / SLEEP_CLK_HZ * 1000u)	/* about 2.3 hours */

#ifndef __ASSEMBLER__
enum spm_mode { SPM_CLOCK_GATING, SPM_POWER_COLLAPSE };
void spm_cpu_mode(enum spm_mode mode);
uint32_t gic_cpu_save(void);
void gic_cpu_restore(uint32_t ppis);
void spmi_init(void);
uint32_t spmi_version(void);
int spmi_read(uint32_t addr, uint8_t *buf, unsigned n);
int spmi_write(uint32_t addr, const uint8_t *buf, unsigned n);
int spmi_apid(uint32_t sid, uint32_t periph);
void spmi_irq_enable(int apid, bool on);
uint32_t spmi_irq_status(int apid);
void spmi_irq_clear(int apid, uint32_t bits);
void pmic_init(void);
uint32_t pmic_battery_mv(void);
int pmic_vadc_raw(uint8_t chan, uint8_t settle);
int pmic_iadc_gain(void);
const char *pmic_charger_status(void);
bool pmic_charging(void);
enum { BAT_DISCHARGING, BAT_CHARGING, BAT_FULL };
int pmic_battery_status(void);
int32_t pmic_battery_ua(void);
int32_t pmic_battery_avg_ua(void);
void pmic_iadc_trim(void);
void pmic_charger_configure(void);
void pmic_charger_poll(void);
void bms_init(void);
void bms_poll(void);
uint32_t bms_soc(void);
uint32_t bms_percent(void);
#define RPM_LDOA 0x616f646cu
#define RPM_SMPA 0x61706d73u
#define RPM_VSA 0x00617376u
bool rpm_init(void);
uint32_t smem_hw_platform(void);
int rpm_request(uint32_t type, uint32_t id, const uint32_t *kvp, unsigned nkvp);
int rpm_regulator_enable(uint32_t type, uint32_t id, bool on);
int rpm_regulator_voltage(uint32_t type, uint32_t id, uint32_t uv);
int rpm_regulator_sleep_enable(uint32_t type, uint32_t id, bool on);
int rpm_clock_rate(uint32_t type, uint32_t id, uint32_t khz);
void rpm_sleep_votes(void);
void rpm_unused_rails_off(void);
void rpm_bus_votes(void);
void dload_clear(void);
int scm_call2(uint32_t svc, uint32_t cmd, uint32_t arg1, uint32_t arg2);
int scm_wdog_debug_disable(void);
int scm_restore_sec_cfg(uint32_t device, uint32_t bank);
#endif

#endif
