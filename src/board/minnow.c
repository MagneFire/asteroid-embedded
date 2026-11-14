/* Moto 360 2014 (minnow), an OMAP3630 watch. The pins are those of
 * hardware revision 9, the watch as it was sold. */
#include "asteroid.h"
#include "sensors/m4_proto.h"
#include "soc/max17050.h"

/* The cell as the vendor's gauge driver programs it when the bootloader
 * names none, which is how the watches come. */
static const struct max17050_model cell_model = {
	.config = 0x6254, .learn_cfg = 0x2606, .filter_cfg = 0x87a4, .relax_cfg = 0x043b, .full_soc_thresh = 0x5f00,
	.design_cap = 606, .fullcap = 662, .fullcapnom = 662, .ichgt_term = 0x0066,
	.rcomp0 = 0x0052, .tempco = 0x333a, .qrtbl = { 0x1b94, 0x0e94, 0x0594, 0x0294 },
	.tgain = 0xe71c, .toff = 0x251a, .version = 0x0001,
	.cell = {
		0x9cf0, 0xa310, 0xb180, 0xb360, 0xb6a0, 0xb6e0, 0xba10, 0xbb10, 0xbbd0, 0xbc10, 0xc200, 0xc250,
		0xc7c0, 0xc820, 0xd050, 0xd710, 0x0140, 0x0110, 0x0210, 0x02c0, 0x1000, 0x0a00, 0x06c0, 0x31a0,
		0x0af0, 0x1170, 0x0cb0, 0x08d0, 0x0aa0, 0x0880, 0x07b0, 0x07b0, 0x0100, 0x0100, 0x0100, 0x0100,
		0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
	},
};

/* The pads as the vendor's kernel sets them when it starts, as the
 * bootloader leaves most of the board's lines unconnected: the GPIOs
 * the drivers here use, the lines that have to wake the chip with
 * their wake-up bit, and the buses with their pulls. The ones from
 * 0x9d0 are in the wake-up domain. */
static const struct board_pad pads[] = {
	{ 0x04a, 0x000f }, { 0x04c, 0x000f }, { 0x04e, 0x000f }, { 0x056, 0x000f }, { 0x058, 0x000f }, { 0x06e, 0x000f },
	{ 0x070, 0x000f }, { 0x072, 0x000f }, { 0x074, 0x000f }, { 0x076, 0x000f }, { 0x078, 0x000f }, { 0x07a, 0x000f },
	{ 0x07c, 0x000f }, { 0x080, 0x000f }, { 0x082, 0x000f }, { 0x084, 0x000f }, { 0x086, 0x000f }, { 0x088, 0x000f },
	{ 0x08a, 0x000f }, { 0x08c, 0x000f }, { 0x08e, 0x4104 }, { 0x096, 0x000f }, { 0x098, 0x000f }, { 0x09a, 0x000f },
	{ 0x09e, 0x000f }, { 0x0a0, 0x0002 }, { 0x0a2, 0x4102 }, { 0x0a4, 0x010f }, { 0x0a6, 0x010f }, { 0x0a8, 0x010f },
	{ 0x0ac, 0x0001 }, { 0x0ae, 0x0001 }, { 0x0b0, 0x0001 }, { 0x0b2, 0x0001 }, { 0x0ba, 0x0004 }, { 0x0bc, 0x0004 },
	{ 0x0c0, 0x0107 }, { 0x0c2, 0x0004 }, { 0x0c6, 0x0004 }, { 0x0c8, 0x0004 }, { 0x0cc, 0x0004 }, { 0x0ce, 0x0004 },
	{ 0x0dc, 0x0004 }, { 0x0e4, 0x0004 }, { 0x0e6, 0x411c }, { 0x0ea, 0x010f }, { 0x102, 0x000f }, { 0x122, 0x0004 },
	{ 0x124, 0x000f }, { 0x126, 0x0004 }, { 0x13c, 0x411c }, { 0x13e, 0x000f }, { 0x140, 0x4104 }, { 0x144, 0x4100 },
	{ 0x146, 0x0000 }, { 0x148, 0x0000 }, { 0x14a, 0x0100 }, { 0x15c, 0x010f }, { 0x168, 0x0104 }, { 0x16a, 0x010f },
	{ 0x16c, 0x0004 }, { 0x16e, 0x4118 }, { 0x170, 0x0000 }, { 0x18e, 0x0118 }, { 0x190, 0x0118 }, { 0x192, 0x0118 },
	{ 0x194, 0x0118 }, { 0x198, 0x0100 }, { 0x19a, 0x0000 }, { 0x19c, 0x0100 }, { 0x19e, 0x0000 }, { 0x1a0, 0x0000 },
	{ 0x1a2, 0x4104 }, { 0x1a4, 0x0004 }, { 0x1ae, 0x0004 }, { 0x1b0, 0x4104 }, { 0x1b2, 0x0004 }, { 0x5a8, 0x011a },
	{ 0x5aa, 0x011a }, { 0x5ae, 0x0004 }, { 0x5b0, 0x0004 }, { 0x5b2, 0x011a }, { 0x5b4, 0x011a }, { 0x5b6, 0x011a },
	{ 0x5b8, 0x011a }, { 0x5ba, 0x0004 }, { 0x5bc, 0x411c }, { 0x5c0, 0x010c }, { 0x5c6, 0x0004 }, { 0x5c8, 0x0004 },
	{ 0x9d6, 0x0008 }, { 0x9e8, 0x0000 }, { 0x9ea, 0x010c }, { 0xa2a, 0x0004 },
	{ 0, 0 },
};

/* The disk has an old-style partition table, so no names of its own. */
static const char *const part_names[] = {
	"", "mbm", "", "ebr", "cdt", "pds", "utags", "logo", "boot", "recovery", "misc", "", "", "system", "cache", "customize",
	"userdata",
};

/* The pads that can wake the chip from its deep sleep, by the number
 * the drivers ask for them with. */
enum { WAKE_NONE, WAKE_BT, WAKE_HUB, WAKE_TOUCH, WAKE_VBUS, WAKE_DOCK, WAKE_CHARGE, WAKE_GAUGE, WAKE_PMIC };
static const uint16_t wake_pads[] = {
	[WAKE_BT] = 0x144, [WAKE_HUB] = 0x08e, [WAKE_TOUCH] = 0x0e6, [WAKE_VBUS] = 0x140, [WAKE_DOCK] = 0x5bc,
	[WAKE_CHARGE] = 0x1a2, [WAKE_GAUGE] = 0x13c, [WAKE_PMIC] = 0x1b0,
};

static const char *const firmware_dirs[] = { "/etc/firmware/", NULL };

const struct board board_minnow = {
	.name = "minnow",
	.compatible = "mot,omap3-minnow",
	.pads = pads,
	.wake_pads = wake_pads,
	.wake_pad_count = ARRAY_SIZE(wake_pads),
	.heart_rate = true,
	/* OTM3201 panel behind an SSD2848 bridge, a round 320 px with the
	 * bottom cut off at 290 lines, scanned at 60 Hz. Reset on gpio 14
	 * and its supply LDO7; the bridge's reset on gpio 23, its 1.2 V
	 * switched by gpio 86, its 1.8 V by gpio 42 and its memory by
	 * gpio 87. */
	.panel = {
		.width = 320, .height = 290,
		.hz = 60, .round = true, .lcd = true, .edge_px = 64,
		.reset_gpio = 14, .io_ldo = 7, .io_uv = 2850000,
		.bridge_reset = 23, .bridge_supply = 86, .vio_gpio = 42, .vio_pad = 0x05a, .mem_gpio = 87,
	},
	/* M4 hub on I2C2, its interrupt on gpio 59, reset on gpio 129 and
	 * boot0 on gpio 21. gpio 28 is a leftover of an earlier design
	 * that the vendor's driver holds low. The hub has the clock and
	 * the light sensor. */
	.hub = &hub_m4,
	.hub_pins = { .i2c = { .bus = 2, .addr = 0x39 }, .irq = 59, .reset = 129, .boot0 = 21, .mpm = WAKE_HUB, .hold_low = 28 },
	.hub_variant = &m4_minnow,
	.light = &light_m4,
	.rsense_uohm = 10000,
	/* MAX17050 on I2C3. The wireless receiver's charging line is gpio
	 * 176, the line it pulses on the pad gpio 22, and gpio 78 high
	 * stops it charging. The factory kill line, gpio 182, is left as
	 * the bootloader has it: the vendor's kernel never drives it. */
	.battery_driver = &battery_max17050,
	.gauge = { .i2c = { .bus = 3, .addr = 0x36 }, .dock = 22, .dock_mpm = WAKE_DOCK, .dock_pulses = true, .charging = 176,
		   .charge_term = 78, .model = &cell_model, .usb_disturbs = true },
	/* Atmel mXT on I2C1, interrupt on gpio 99, reset on gpio 164. */
	.touch = &touch_mxt,
	.touch_pins = { .i2c = { .bus = 1, .addr = 0x4a }, .irq = 99, .reset = 164, .mpm = WAKE_TOUCH, .ldo = 8, .ldo_uv = 2800000,
			.own_settings = true },
	/* LM3535 on I2C3 next to the gauge, enable on gpio 98. */
	.backlight = &backlight_lm3535,
	.backlight_pins = { .i2c = { .bus = 3, .addr = 0x38 }, .enable = 98 },
	/* The vendor's ambient maximum. */
	.backlight_aod_code = 36,
	/* The motor runs from LDO6. */
	.motor = &motor_ldo,
	.motor_pins = { .ldo = 6, .ldo_uv = 2400000 },
	.periph_clock_gpio = 177,
	.usb_phy = { .vbus = 142, .resetn = 15, .cs = 16, .csn = 27 },
	.part_names = part_names,
	.part_count = ARRAY_SIZE(part_names),
	.firmware_dirs = firmware_dirs,
	/* WiLink on UART2 at 3 Mbaud, enabled by gpio 83; its RTS, the
	 * UART's CTS, is what wakes the watch. */
	.bt = { .uart = 2, .reg_on = 83, .mpm = WAKE_BT, .patch = "TIInit_11.8.32.bts",
		.addr_part = "pds", .addr_path = "bluetooth/bdaddr.txt" },
};
