/* The SoC's side of USB: the controller's clocks, its PHY and its
 * interrupt, around the stack that drives the controller. */
#ifndef USB_PORT_H
#define USB_PORT_H
#include <stdbool.h>

/* At boot, with the block as the bootloader left it; false if the
 * controller did not come out of its reset. */
bool usb_port_init(void);
/* What the stack's own controller reset undid, between its start and
 * the device attaching; 0 or what failed. */
const char *usb_port_attach(void);
/* A host is attached and the bus is not suspended. */
bool usb_port_connected(void);
/* Unplugged: the PHY to its low power mode and the clocks off, with the
 * stack stopped already. False if the PHY refused and the clocks stay
 * on. Nothing may touch the controller until usb_port_restore. */
bool usb_port_rest(void);
bool usb_port_resting(void);
/* From rest back to where the stack can start; 0 or what failed, with
 * the block at rest again where it could not be reached. */
const char *usb_port_restore(void);
#endif
