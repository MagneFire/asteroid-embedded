/* A vibration motor behind a driver chip that takes its drive as the duty
 * cycle of one of the camera's general purpose clocks, GP0 or GP1, on a
 * pin, with its own enable line and, where nothing else keeps it on, its
 * supply switched with it. The clock runs from XO divided by 4 in dual edge mode, its
 * N sets the PWM period and D the duty cycle around half, which is zero
 * drive; the values are the vendor's. The motor starts with full drive for
 * DRIVE_MS, as the vendor's driver does, then runs at the board's level;
 * the wait is short enough to spend in place. */
#include "asteroid.h"

#define GP_CMD_RCGR	(MMSS_CC_BASE + (board->motor_pins.pwm_gp0 ? 0x3420 : 0x3450))
#define GP_CFG_RCGR	(GP_CMD_RCGR + 0x04)
#define GP_M		(GP_CMD_RCGR + 0x08)
#define GP_N		(GP_CMD_RCGR + 0x0c)
#define GP_D		(GP_CMD_RCGR + 0x10)
#define GP_CBCR		(GP_CMD_RCGR + 0x24)
#define CFG_DUAL_EDGE_XO_DIV4	(2 << 12 | 0 << 8 | 7 << 0)
#define CMD_UPDATE	BIT(0)
#define D_HALF		64
#define DRIVE_MS	20

#define GPIO_PWM	(board->motor_pins.pwm_gpio)
#define GPIO_ENABLE	(board->motor_pins.enable)

static bool on;

/* The vendor's mapping of a drive in percent to D, from half (no drive)
 * up to two below the full period. */
static uint32_t duty(unsigned percent)
{
	unsigned level = (2 * percent * (D_HALF - 2) + 100) / 200;
	if (!level && percent)
		level = 1;
	return D_HALF + level;
}

static void clock_set(unsigned percent)
{
	writel(CFG_DUAL_EDGE_XO_DIV4, GP_CFG_RCGR);
	writel(1, GP_M);
	writel(~(board->motor_pins.pwm_n - 1u) & 0xff, GP_N);
	writel(~(duty(percent) << 1) & 0xff, GP_D);
	writel(CMD_UPDATE, GP_CMD_RCGR);
}

static bool gp_init(void)
{
	if (!GPIO_PWM)
		return false;
	gpio_set(GPIO_ENABLE, 0);
	gpio_config(GPIO_ENABLE, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_config(GPIO_PWM, board->motor_pins.pwm_func, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	writel(0, GP_CBCR);
	log_printf("haptics: gp%u pwm on gpio %u, n %u, %u%%, cbcr %08lx\n", board->motor_pins.pwm_gp0 ? 0 : 1, GPIO_PWM, board->motor_pins.pwm_n,
		   board->motor_pins.pwm_percent, readl(GP_CBCR));
	return true;
}

static void gp_set(bool want)
{
	if (want == on)
		return;
	on = want;
	if (!want) {
		gpio_set(GPIO_ENABLE, 0);
		writel(0, GP_CBCR);
		if (board->motor_pins.ldo)
			regulator_off(board->motor_pins.ldo);
		return;
	}
	if (board->motor_pins.ldo)
		regulator_on(board->motor_pins.ldo, board->motor_pins.ldo_uv);
	clock_set(100);
	writel(1, GP_CBCR);
	gpio_set(GPIO_ENABLE, 1);
	delay_ms(DRIVE_MS);
	clock_set(board->motor_pins.pwm_percent);
}

const struct motor_driver motor_gp_pwm = { .init = gp_init, .set = gp_set };
