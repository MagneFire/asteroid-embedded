/* APQ8026 / MSM8226 register map used by the firmware. */
#ifndef MSM8226_H
#define MSM8226_H

#define GICD_BASE		0xf9000000u
#define GICC_BASE		0xf9002000u
#define APCS_WDT_BASE		0xf9017000u
#define SLEEP_CLK_HZ		32768u

#define IMEM_BASE		0xfe805000u
#define IMEM_DLOAD_MAGIC	(IMEM_BASE + 0x000)
#define IMEM_EDLOAD_MAGIC	(IMEM_BASE + 0xfe0)
#define IMEM_RESTART_REASON	(IMEM_BASE + 0x65c)

#define MPM2_PS_HOLD		0xfc4ab000u

#define SPMI_BASE		0xfc4c0000u
#define SPMI_ARB_VERSION	(SPMI_BASE + 0xf000)
#define SPMI_ARB_CHNL(n)	(SPMI_BASE + 0xf800 + (n) * 0x80)
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
#define MDSS_BASE		0xfd900000u
#define MDP_HW_REV		(MDSS_BASE + 0x0100)
#define MDP_CTL0		(MDSS_BASE + 0x0600)
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
#define MDP_PP0			(MDSS_BASE + 0x21b00)
#define PP_SYNC_CONFIG_VSYNC	0x04
#define PP_AUTOREFRESH_CONFIG	0x30

#define IRQ_PPI_TIMER_S		18
#define IRQ_PPI_TIMER		19
#define IRQ_WDT_BARK		(32 + 3)

#define RAMOOPS_BASE		0x1ef80000u
#define RAMOOPS_SIZE		0x80000u
#define RAMOOPS_RECORD_SIZE	0x2000u
#define RAMOOPS_CONSOLE_SIZE	0x40000u

#endif
