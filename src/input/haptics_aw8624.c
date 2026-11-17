/* AW8624 haptics driver on beluga, run as a plain motor: its RAM mode
 * would play stored waveforms, but the patterns here are on/off times,
 * so the chip is reset, its ID checked and it is switched between
 * standby and its continuous mode. */
#include "asteroid.h"

#define BUS		(board->motor_pins.i2c.bus)
#define ADDR		(board->motor_pins.i2c.addr)
#define REG_ID		0x00
#define REG_SYSCTRL	0x04
#define REG_GO		0x05
#define CHIP_ID		0x24
#define SYSCTRL_STANDBY	0x01
#define SYSCTRL_CONT	(2u << 2)
#define GO_START	0x01
#define GO_STOP		0x00

static bool up;

static void let_go(void)
{
	if (board->motor_pins.enable)
		gpio_config(board->motor_pins.enable, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
}

static int write(uint8_t reg, uint8_t val)
{
	uint8_t w[2] = { reg, val };
	return i2c_xfer(BUS, ADDR, w, 2, NULL, 0);
}

static bool aw_init(void)
{
	if (board->motor_pins.enable) {
		gpio_set(board->motor_pins.enable, 0);
		gpio_config(board->motor_pins.enable, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
		delay_ms(1);
		gpio_set(board->motor_pins.enable, 1);
		delay_ms(5);
	}
	uint8_t reg = REG_ID, id = 0;
	up = !i2c_init(BUS, board->motor_pins.i2c.sda, board->motor_pins.i2c.scl, board->motor_pins.i2c.func, 400000) &&
	     !i2c_xfer(BUS, ADDR, &reg, 1, &id, 1) && id == CHIP_ID;
	if (!up) {
		log_printf("haptics: no AW8624 (id %02x)\n", id);
		let_go();
		return false;
	}
	write(REG_SYSCTRL, SYSCTRL_STANDBY);
	return true;
}

static void aw_set(bool on)
{
	if (on) {
		write(REG_SYSCTRL, SYSCTRL_CONT);
		write(REG_GO, GO_START);
	} else {
		write(REG_GO, GO_STOP);
		write(REG_SYSCTRL, SYSCTRL_STANDBY);
	}
}

/* As stock: the enable undriven while the AP has the chip, and all of
 * the start again when it comes back. */
static bool aw_lend(bool to_ap)
{
	if (!to_ap)
		return aw_init();
	if (up)
		aw_set(false);
	let_go();
	return false;
}

const struct motor_driver motor_aw8624 = { .init = aw_init, .set = aw_set, .lend = aw_lend };
