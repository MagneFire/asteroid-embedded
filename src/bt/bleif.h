/* The BLE interface's link to the controller (bleif.c), and the HCI layer
 * over it (bleif_hci.c). Packets are H4, the type byte first, in
 * word-aligned buffers. */
#ifndef BLEIF_H
#define BLEIF_H
#include <stdbool.h>
#include <stdint.h>

/* The longest packet either way: an event of 255 bytes, rounded up. */
#define BLEIF_PACKET_WORDS 65

/* 0, or -1 when the controller did not take it: it may want to send
 * first, so read what it has and try again. */
int bleif_link_write(const uint32_t *w, unsigned n);
bool bleif_link_pending(void);
/* The packet's length, 0 for none, or -1 on an error. */
int bleif_link_read(uint32_t *w, unsigned max);

/* Read what the controller has and hand it on; true if there was any. */
bool bleif_hci_service(void);
bool bleif_hci_idle(void);
/* Sent with VS_SET_BD_ADDR after every HCI Reset of the host. */
void bleif_hci_set_addr(const uint8_t lsb[6]);
#define VS_SET_BD_ADDR		0xfc32
#endif
