/* The ChipIdea high-speed controller of the APQ8026 at 0xf9a55000 (the
 * register struct starts 0x100 before the capability registers),
 * interrupt 166. */
#ifndef CI_HS_MSM_H
#define CI_HS_MSM_H
#include "asteroid.h"

static const ci_hs_controller_t _ci_controller[] = {
	{ .reg_base = USB_HS_BASE, .irqnum = IRQ_USB_HS },
};

#define CI_HS_REG(_port)	((ci_hs_regs_t *)(uintptr_t)_ci_controller[_port].reg_base)
#define CI_DCD_INT_ENABLE(_p)	gic_unmask(_ci_controller[_p].irqnum)
#define CI_DCD_INT_DISABLE(_p)	gic_mask(_ci_controller[_p].irqnum)

#endif
