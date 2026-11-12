/* What differs between the Qualcomm chips the image runs on: where each
 * block is and a few of its settings. The registers inside a block are
 * the same on both. Addresses are as the firmware sees them: on the
 * APQ8009W, IO_8009_BASE above the physical one. */
#ifndef CHIP_H
#define CHIP_H
#include <stdbool.h>
#include <stdint.h>

struct qcom_chip {
	const char *name;
	uint32_t io_offset;		/* add to a physical peripheral address */
	uint32_t apcs;			/* GIC, timers, watchdog, CPU clock, SAWs */
	uint32_t imem, ps_hold, smem, rpm_msg_ram, handoff_lock;
	uint32_t spmi_core, spmi_cnfg, spmi_intr, spmi_chnls, spmi_obsrvr;
	uint32_t tlmm_pins, tlmm_stride, sdc1_pads;
	unsigned gpios;
	uint32_t gcc, gpll0_vote, gpll0_status, gpll0_ready, blsp_vote, blsp_vote_bit, sleep_vote;
	uint32_t gpll0_khz;
	uint32_t qup1;
	uint16_t qup_i2c_rcgr[6], qup_i2c_cbcr[6], qup_spi_rcgr[6], qup_spi_cbcr[6], qup_bcr[6];
	uint32_t uart1;
	unsigned uarts;
	uint16_t uart_cbcr[6], uart_rcgr[6];
	uint8_t uart2_pins[4], uart2_funcs[4];	/* TX, RX, CTS, RFR when not in a row */
	uint32_t sdcc1;
	uint32_t sdcc1_apps_cbcr, sdcc1_ahb_cbcr, sdcc1_rcgr;
	struct { uint8_t ldo; uint32_t uv; } emmc_vcc, emmc_vccq;
	uint32_t usb, usb_bcr, usb_system_cbcr, usb_ahb_cbcr, usb_system_rcgr, usb_phy_bcr;
	uint32_t dsi;
	uint32_t mdp3;			/* 0 with the MDP5 */
	uint32_t gpu;
	bool cnoc;			/* has the RPM's CNOC bus clock */
	bool imem_dload;		/* download-mode cookies in IMEM */
	bool pon_restart_reason;	/* the bootloader reads it from the PMIC */
};

extern const struct qcom_chip chip_8026, chip_8009;
extern const struct qcom_chip *chip;
void chip_init(void);

#endif
