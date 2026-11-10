/* TinyUSB's driver for the Mentor controller, built for the OMAP3's:
 * the driver takes its port from a header for the microcontrollers it
 * knows, so that header is kept out and its part is given here. The
 * controller's FIFO memory is 16 KiB, shared out as endpoints open. */
#include "asteroid.h"
#include "tusb_option.h"

#define TUSB_MUSB_TI_H_
#define MUSB_CFG_SHARED_FIFO		0
#define MUSB_CFG_DYNAMIC_FIFO		1
#define MUSB_CFG_DYNAMIC_FIFO_SIZE	16384

#define POWER		(MUSB_BASE + 0x01)
#define INTRUSBE	(MUSB_BASE + 0x0b)
#define TESTMODE	(MUSB_BASE + 0x0f)
#define DEVCTL		(MUSB_BASE + 0x60)
#define POWER_HSENAB	BIT(5)
#define DEVCTL_SESSION	BIT(0)
#define DEVCTL_VBUS_VALID	(3u << 3)
#define INTR_SUSPEND	BIT(0)
#define INTR_RESUME	BIT(1)
#define INTR_RESET	BIT(2)

static const uintptr_t MUSB_BASES[] = { MUSB_BASE };
static bool irq_on;

static inline uint8_t rb(uint32_t a) { return *(volatile uint8_t *)(uintptr_t)a; }
static inline void wb(uint8_t v, uint32_t a) { *(volatile uint8_t *)(uintptr_t)a = v; }

/* The core as the stack expects it after a reset of its own, which a
 * bootloader that used it has not left: no test mode and only the bus
 * interrupts. A session is asked for only without VBUS, as the vendor's
 * driver does: with VBUS the controller starts one itself, and asking
 * then keeps it from attaching. */
static inline void musb_dcd_phy_init(uint8_t rhport)
{
	wb(0, TESTMODE);
	wb(INTR_SUSPEND | INTR_RESUME | INTR_RESET, INTRUSBE);
	wb(POWER_HSENAB, POWER);
	if ((rb(DEVCTL) & DEVCTL_VBUS_VALID) != DEVCTL_VBUS_VALID)
		wb(rb(DEVCTL) | DEVCTL_SESSION, DEVCTL);
}

static inline void musb_dcd_int_enable(uint8_t rhport)
{
	irq_on = true;
	irq_unmask(USB_IRQ);
}

static inline void musb_dcd_int_disable(uint8_t rhport)
{
	irq_mask(USB_IRQ);
	irq_on = false;
}

static inline unsigned musb_dcd_get_int_enable(uint8_t rhport) { return irq_on; }
static inline void musb_dcd_int_clear(uint8_t rhport) {}
static inline void musb_dcd_int_handler_enter(uint8_t rhport) {}

/* The driver times its 10 ms of resume signalling by counting. */
#define SystemCoreClock	10000u
#define __NOP()		delay_us(1000)

#include "portable/mentor/musb/dcd_musb.c"

bool dcd_stuck(void) { return false; }

/* Without it the stack counts as still started and the next start does
 * nothing. */
bool dcd_deinit(uint8_t rhport)
{
	musb_dcd_int_disable(rhport);
	return true;
}
