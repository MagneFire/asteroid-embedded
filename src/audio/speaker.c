/* The speaker amplifier: an NXP TFA9895 at 0x34 on BLSP1 QUP3 (gpio
 * 10 and 11), the address and type in the vendor's tuning container.
 * Its registers are 16 bits, big-endian; register 3 holds the
 * revision, 0x12 for the TFA9895. */
#include "asteroid.h"
#include "speaker.h"

#define QUP		(board->speaker.qup)
#define ADDR		(board->speaker.addr)
#define REG_REV		0x03

static int read_reg(uint8_t reg, uint16_t *val)
{
	uint8_t b[2];
	int r = i2c_qup_xfer(QUP, ADDR, &reg, 1, b, 2);
	if (!r)
		*val = (uint16_t)(b[0] << 8 | b[1]);
	return r;
}

int speaker_probe(uint16_t *rev)
{
	static bool bus;
	int r;
	if (!ADDR)
		return -1;
	if (!bus && (r = i2c_qup_init(QUP, board->speaker.sda, board->speaker.scl, board->speaker.func, 384000)))
		return r;
	bus = true;
	return read_reg(REG_REV, rev);
}
