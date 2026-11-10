/* A vibration motor that runs while its supply is on. */
#include "asteroid.h"

static bool ldo_init(void)
{
	return board->motor_pins.ldo != 0;
}

static void ldo_set(bool on)
{
	if (on)
		regulator_on(board->motor_pins.ldo, board->motor_pins.ldo_uv);
	else
		regulator_off(board->motor_pins.ldo);
}

const struct motor_driver motor_ldo = { .init = ldo_init, .set = ldo_set };
