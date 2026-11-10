/* The host controller under the eMMC driver, one per SoC. A command is
 * an index, an argument and SDHCI's command register bits for the kind
 * of response, which both controllers here take as they are. Data goes
 * through emmc_bounce. */
#ifndef EMMC_HOST_H
#define EMMC_HOST_H
#include "emmc.h"

#define CMD_RESP_NONE		0u
#define CMD_RESP_136		1u
#define CMD_RESP_48		2u
#define CMD_RESP_48_BUSY	3u
#define CMD_CRC			BIT(3)
#define CMD_INDEX		BIT(4)
#define CMD_DATA		BIT(5)

#define R_NONE			CMD_RESP_NONE
#define R1			(CMD_RESP_48 | CMD_CRC | CMD_INDEX)
#define R1B			(CMD_RESP_48_BUSY | CMD_CRC | CMD_INDEX)
#define R2			(CMD_RESP_136 | CMD_CRC)
#define R3			CMD_RESP_48


#define EMMC_XFER_BLOCKS	64u

struct emmc_cmd {
	uint32_t index, arg, flags;
	/* A 136-bit response has its top word last. */
	uint32_t resp[4];
	uint32_t blocks, blksz;
	bool read;
	uint32_t timeout_us;
};

extern uint8_t emmc_bounce[EMMC_XFER_BLOCKS * EMMC_BLOCK];

/* Powered and clocked at 400 kHz, ready for CMD0; what it finds out
 * about itself goes into card. */
int emmc_host_init(struct emmc_info *card);
int emmc_host_send(struct emmc_cmd *c);
/* The card clock, as near below hz as the host gets; what it is goes
 * into *actual. */
int emmc_host_clock(uint32_t hz, uint32_t *actual);
/* The command line open drain, as cards want it until they have an
 * address. */
void emmc_host_open_drain(bool on);
/* High speed timing, before the clock goes up. */
void emmc_host_high_speed(void);
/* The widest bus the host has wired, and the width to use. */
unsigned emmc_host_max_width(void);
void emmc_host_set_width(unsigned bits);
/* The card's clock and the controller's own off while the card sleeps,
 * and back; nonzero if they did not come back. */
int emmc_host_rest(bool on);
#endif
