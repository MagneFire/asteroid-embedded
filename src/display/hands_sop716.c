/* Soprod SOP716: the microcontroller that drives narwhal's two hands and
 * keeps the local time for them, also with the rest of the watch off.
 * A command is written as its size, then its id and that many bytes
 * less one; a read names the id and gets the size back first. Reads less
 * than 100 ms apart fail, as the vendor found. Motor 1 is the minute
 * hand, motor 2 the hour hand, each 180 steps to a turn.
 *
 * In watch mode the hands follow the controller's own clock. Moving
 * them leaves it until a time or the watch mode command brings them
 * back. A failed write resets the controller, as the vendor does, which
 * keeps its time. */
#include "asteroid.h"

#define BUS		(board->hands_pins.i2c.bus)
#define ADDR		(board->hands_pins.i2c.addr)
#define BUS_HZ		400000

enum {
	CMD_SET_TIME = 0,
	CMD_MOVE_ALL = 2,
	CMD_READ_TIME = 4,
	CMD_READ_VERSION = 5,
	CMD_CHECK_INTERVAL = 7,
	CMD_READ_BATTERY = 8,
	CMD_SET_EOL = 12,
	CMD_READ_POSITION = 14,
	CMD_READ_MODE = 15,
};
/* Each command's size, and the firmware revision it needs. */
static const uint8_t size[] = { 8, 4, 4, 4, 6, 2, 0, 3, 2, 2, 3, 2, 5, 4, 2, 1 };
static const uint8_t needs_rev[] = { [CMD_READ_POSITION] = 16, [CMD_READ_MODE] = 16 };

#define OPT_NO_HANDS_MOVE	0x08
#define MODE_WATCH		5
#define READ_GAP_MS		100
#define RESET_MS		10
/* The vendor's tree: time after a reset, its battery check every 30 s
 * and the cell levels below which the hands stop. */
#define BOOT_MS			1000
#define CHECK_INTERVAL_S	30
#define EOL_LIMIT_MV		2640
#define EOL_THRESHOLD_MV	3060
/* Minute hand at 3 and hour hand at 9, where stock AsteroidOS puts them
 * first. */
#define ASIDE_MINUTE		45
#define ASIDE_HOUR		135

static bool present, aside;
static void set_aside(bool on);
static uint8_t fw_ver, fw_rev;
static uint32_t read_t;

static void hw_reset(void)
{
	gpio_set(board->hands_pins.reset, 1);
	delay_ms(RESET_MS);
	gpio_set(board->hands_pins.reset, 0);
	delay_ms(BOOT_MS);
}

static int write_cmd(uint8_t cmd, const uint8_t *params)
{
	uint8_t b[9] = { size[cmd], cmd };
	for (unsigned i = 1; i < size[cmd]; i++)
		b[1 + i] = params[i - 1];
	int r = i2c_xfer(BUS, ADDR, b, 1u + size[cmd], NULL, 0);
	if (r) {
		log_printf("hands: command %u failed (%d), resetting\n", cmd, r);
		hw_reset();
		r = i2c_xfer(BUS, ADDR, b, 1u + size[cmd], NULL, 0);
	}
	return r;
}

/* out gets the size bytes after the size. */
static int read_cmd(uint8_t cmd, uint8_t *out)
{
	if (needs_rev[cmd] > fw_rev)
		return -1;
	uint8_t b[9];
	int r = -1;
	for (unsigned tries = 0; tries < 2 && r; tries++) {
		uint32_t gone = time_ms() - read_t;
		if (gone < READ_GAP_MS)
			delay_ms(READ_GAP_MS - gone);
		r = i2c_xfer(BUS, ADDR, &cmd, 1, b, 1u + size[cmd]);
		read_t = time_ms();
		if (!r && b[0] != size[cmd])
			r = -2;
	}
	for (unsigned i = 0; !r && i < size[cmd]; i++)
		out[i] = b[1 + i];
	return r;
}

static bool configure(void)
{
	uint8_t interval[] = { CHECK_INTERVAL_S >> 8, CHECK_INTERVAL_S & 0xff };
	uint8_t eol[] = { EOL_LIMIT_MV >> 8, EOL_LIMIT_MV & 0xff, EOL_THRESHOLD_MV >> 8, EOL_THRESHOLD_MV & 0xff };
	return !write_cmd(CMD_CHECK_INTERVAL, interval) && !write_cmd(CMD_SET_EOL, eol);
}

static bool sop716_init(void)
{
	gpio_set(board->hands_pins.reset, 0);
	gpio_config(board->hands_pins.reset, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_set(board->hands_pins.bslen, 0);
	gpio_config(board->hands_pins.bslen, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	gpio_set(board->hands_pins.temp, 0);
	gpio_config(board->hands_pins.temp, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
	uint8_t v[6] = { 0 };
	int r = i2c_init(BUS, board->hands_pins.i2c.sda, board->hands_pins.i2c.scl, board->hands_pins.i2c.func, BUS_HZ);
	if (!r)
		r = read_cmd(CMD_READ_VERSION, v);
	if (r) {
		log_printf("hands: sop716 not found (%d)\n", r);
		return false;
	}
	present = true;
	fw_ver = v[0];
	fw_rev = v[1];
	uint8_t mode = MODE_WATCH, t[6] = { 0 };
	read_cmd(CMD_READ_MODE, &mode);
	read_cmd(CMD_READ_TIME, t);
	struct civil c;
	civil_from_epoch(rtc_read(), &c);
	log_printf("hands: sop716 firmware %u.%u, mode %u, its time %02u:%02u:%02u, ours %02lu:%02lu:%02lu\n", fw_ver,
		   fw_rev, mode, t[0], t[1], t[2], c.hour, c.minute, c.second);
	if (!configure())
		log_printf("hands: battery levels not set\n");
	if (mode != MODE_WATCH)
		set_aside(false);
	return true;
}

static void set_time(void)
{
	if (!present)
		return;
	struct civil c;
	civil_from_epoch(rtc_read(), &c);
	uint8_t p[] = { (uint8_t)c.hour, (uint8_t)c.minute, (uint8_t)c.second, (uint8_t)(c.year - 2000),
			(uint8_t)c.month, (uint8_t)c.day, aside ? OPT_NO_HANDS_MOVE : 0 };
	if (write_cmd(CMD_SET_TIME, p))
		log_printf("hands: time not set\n");
}

static void set_aside(bool on)
{
	if (!present)
		return;
	int r;
	if (on) {
		uint8_t p[] = { ASIDE_MINUTE, ASIDE_HOUR, 0 };
		r = write_cmd(CMD_MOVE_ALL, p);
	} else {
		/* A time of 255:255 is the watch mode command. */
		uint8_t p[7] = { 0xff, 0xff };
		r = write_cmd(CMD_SET_TIME, p);
	}
	if (r)
		log_printf("hands: not moved %s\n", on ? "aside" : "to the time");
	else
		aside = on;
}

static void status(void (*say)(const char *fmt, ...))
{
	if (!present) {
		say("hands: none\n");
		return;
	}
	uint8_t mode = 0, t[6] = { 0 }, pos[2] = { 0 }, batt[2] = { 0 };
	int rm = read_cmd(CMD_READ_MODE, &mode), rt = read_cmd(CMD_READ_TIME, t);
	int rp = read_cmd(CMD_READ_POSITION, pos), rb = read_cmd(CMD_READ_BATTERY, batt);
	say("hands: sop716 %u.%u, %s, mode %u (%d), time 20%02u-%02u-%02u %02u:%02u:%02u (%d), minute %u hour %u (%d), "
	    "battery %u mV (%d)\n",
	    fw_ver, fw_rev, aside ? "aside" : "telling the time", mode, rm, t[3], t[4], t[5], t[0], t[1], t[2], rt, pos[0],
	    pos[1], rp, batt[0] << 8 | batt[1], rb);
}

static void move(uint8_t minute, uint8_t hour)
{
	uint8_t p[] = { minute, hour, 0 };
	if (present && !write_cmd(CMD_MOVE_ALL, p))
		aside = true;
}

const struct hands_driver hands_sop716 = {
	.init = sop716_init, .set_time = set_time, .aside = set_aside, .status = status, .move = move,
};
