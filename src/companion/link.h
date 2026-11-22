/* The link to the application processor over the IO slave, one packet
 * at a time each way (wire.h has the framing). The AP asks to write
 * with dn_int; once that has been high for LINK_DN_SETTLE_MS, dn_ack
 * goes high while it may, and low again within 30 ms of a good packet
 * or after LINK_ERROR_MS for a bad one. A packet for the AP is staged,
 * then up_int goes high until the AP has read it
 * and answered on up_ack: a short pulse for a good packet, one of 30 ms
 * for a bad one. The interrupts do the transfers; link_poll keeps the
 * deadlines and starts what is waiting. */
#ifndef COMPANION_LINK_H
#define COMPANION_LINK_H
#include <stdbool.h>
#include <stdint.h>

/* The vendor's waits, in its 1024 Hz ticks: 500 for a packet from the
 * AP, 1500 for the AP to read one, 45 for the error hold, and an
 * answer from the AP of 20 or more is an error. */
#define LINK_RX_MS		488
#define LINK_TX_MS		1465
#define LINK_ERROR_MS		44
#define LINK_ACK_ERROR_US	19531
#define LINK_ACK_STUCK_MS	100
#define LINK_TX_TRIES		2
/* The AP's boot loader raises dn_int for about 80 ms and reads any
 * answer as the MCU's boot state; the AP's writes hold it until
 * dn_ack. */
#define LINK_DN_SETTLE_MS	150
/* The boot loader's ask comes with up_ack high, its boot notice. It
 * reads the answer within 75 ms and waits 195 ms for it to drop; the
 * vendor drops it after 80 of its ticks. */
#define LINK_LK_ANSWER_MS	78
/* Unfinished packets in a row after which dn_int is ignored until the
 * AP writes, or for LINK_DEAF_MS, twice that after each further one up
 * to LINK_DEAF_MAX_MS. The boot loader's ask is still answered. */
#define LINK_UNFINISHED_MAX	3
#define LINK_DEAF_MS		5000
#define LINK_DEAF_MAX_MS	80000
/* What fits in the direct bytes of one read with the header. */
#define LINK_SEND_MAX		112

enum link_result { LINK_PENDING, LINK_SENT, LINK_FAILED };

void link_init(void);
bool link_ready(void);
void link_poll(void);
/* The payload of the packet the AP wrote last, if one is waiting:
 * its length, or 0. */
unsigned link_recv(uint8_t *buf, unsigned cap);
/* Queues a payload for the AP; false while another is waiting or if
 * it is over LINK_SEND_MAX. */
bool link_send(const uint8_t *p, unsigned n);
/* What became of the last payload queued. */
enum link_result link_sent(void);
/* A transfer is under way. */
bool link_busy(void);
/* How long nothing has happened on the link, 0 while something is. */
uint32_t link_quiet_ms(void);
uint32_t link_next_ms(void);
/* Whether the boot loader's ask is answered: while the watch has the
 * panel, with the AP off. */
void link_answer_lk(bool on);
/* Once for each ask answered that the boot loader held to the end. */
bool link_lk_asked(void);
void link_status(void (*say)(const char *fmt, ...));
#endif
