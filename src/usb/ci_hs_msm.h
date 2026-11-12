/* The ChipIdea high-speed controller (the register struct starts 0x100
 * before the capability registers), interrupt 166 on both chips. */
#ifndef CI_HS_MSM_H
#define CI_HS_MSM_H
#include "asteroid.h"

#define CI_HS_REG(_port)	((ci_hs_regs_t *)(uintptr_t)USB_HS_BASE)
#define CI_DCD_INT_ENABLE(_p)	irq_unmask(IRQ_USB_HS)
#define CI_DCD_INT_DISABLE(_p)	irq_mask(IRQ_USB_HS)

#endif
