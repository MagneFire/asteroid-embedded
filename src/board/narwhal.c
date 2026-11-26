/* LG Watch W7 (narwhal): APQ8009 with a PM8916, the rev-10, rev-c and
 * rev-a boards. Two mechanical hands sit over the LCD. */
#include "asteroid.h"
#include "display/display.h"
#include "soc/max17050.h"

/* The GPU microcode is on the vendor partition. */
static const char *const firmware_dirs[] = { "/firmware/", NULL };

/* The vendor's LM36272 current code for each brightness level. */
static const uint16_t backlight_map[256] = {
	0, 553, 553, 553, 553, 553, 553, 553, 553, 553, 553, 553, 558, 564, 569, 574,
	580, 585, 590, 596, 601, 606, 612, 617, 622, 628, 633, 638, 644, 649, 654, 660,
	665, 670, 676, 681, 686, 692, 697, 702, 708, 713, 718, 724, 729, 734, 740, 745,
	750, 756, 761, 766, 772, 777, 782, 788, 793, 798, 804, 809, 814, 820, 825, 830,
	836, 841, 846, 852, 857, 862, 868, 873, 878, 884, 889, 894, 900, 905, 910, 916,
	921, 926, 932, 937, 942, 948, 953, 958, 964, 969, 974, 980, 985, 990, 996, 1001,
	1006, 1012, 1017, 1022, 1028, 1033, 1038, 1044, 1049, 1054, 1060, 1065, 1070, 1076, 1081, 1086,
	1092, 1097, 1102, 1108, 1113, 1118, 1124, 1129, 1134, 1140, 1145, 1150, 1156, 1161, 1166, 1172,
	1177, 1182, 1188, 1193, 1198, 1204, 1209, 1214, 1220, 1225, 1230, 1236, 1241, 1246, 1252, 1257,
	1262, 1268, 1273, 1278, 1284, 1289, 1294, 1300, 1305, 1310, 1316, 1321, 1326, 1332, 1337, 1342,
	1348, 1353, 1358, 1364, 1369, 1374, 1380, 1385, 1390, 1396, 1401, 1406, 1412, 1417, 1422, 1428,
	1433, 1438, 1444, 1449, 1454, 1460, 1465, 1470, 1476, 1481, 1486, 1492, 1497, 1502, 1508, 1513,
	1518, 1524, 1529, 1534, 1540, 1545, 1550, 1556, 1561, 1566, 1572, 1577, 1582, 1588, 1593, 1598,
	1604, 1609, 1614, 1620, 1625, 1630, 1636, 1641, 1646, 1652, 1657, 1662, 1668, 1673, 1678, 1684,
	1689, 1694, 1700, 1705, 1710, 1716, 1721, 1726, 1732, 1737, 1742, 1748, 1753, 1758, 1764, 1769,
	1774, 1780, 1785, 1790, 1796, 1801, 1806, 1812, 1817, 1822, 1828, 1833, 1838, 1844, 1849, 1885,
};

/* FAN54511 as the vendor's tree sets it, apart from the float voltage,
 * USB input current and charge enable, which the driver keeps: charging
 * starts at 3.0 V, 200 mA, 50 mA pre-charge, the termination current at
 * its highest for the vendor's own end of charge, the battery divider off,
 * 6.5 V over- and 4.56 V under-voltage on both inputs, no fast charge
 * timer, no battery detection or top-off time limit, termination and
 * top-off on. The wireless input is not fitted but set as the vendor does. */
static const struct board_reg charger_setup[] = {
	{ 0x0c, 0x07, (3000 - 2700) / 100 },
	{ 0x12, 0, (200 - 200) / 50 },
	{ 0x13, 0, 15 << 4 | (50 - 50) / 50 },
	{ 0x16, 0, (325 - 325) / 25 },
	{ 0x30, 1 << 5, 0 },
	{ 0x15, 0, 0 << 4 | 4 },
	{ 0x17, 0, 0 << 4 | 4 },
	{ 0x19, 0x07, 0 },
	{ 0x1b, 0x0f, 0x08 },
	{ 0x0e, 1 << 3 | 1 << 2, 1 << 3 | 1 << 2 },
};

/* The vendor's cell model for the 240 mAh cell, from its tree. The gauge
 * takes its temperature from the host. */
static const struct max17050_model cell_model = {
	.config = 0x2104, .learn_cfg = 0x2603, .filter_cfg = 0x87a4, .relax_cfg = 0x506b, .full_soc_thresh = 0x5f00,
	.design_cap = 0x01d3, .fullcap = 0x01d3, .fullcapnom = 0x01d3, .ichgt_term = 0x00a0,
	.rcomp0 = 0x0037, .tempco = 0x2d3a, .qrtbl = { 0x2a00, 0x1483, 0x0a00, 0x0780 },
	.tgain = 0xe932, .toff = 0x2381, .version = 0x0004, .v_empty = 0xa05f, .iavg_empty = 0x0175,
	.cell = {
		0x94a0, 0xae60, 0xb740, 0xb850, 0xbac0, 0xbbd0, 0xbd20, 0xbe00, 0xc010, 0xc150, 0xc450, 0xc7e0,
		0xcb00, 0xce40, 0xd330, 0xd970, 0x0050, 0x0220, 0x1780, 0x0e40, 0x1130, 0x0d00, 0x2660, 0x0f10,
		0x1340, 0x0af0, 0x0770, 0x0830, 0x06f0, 0x07e0, 0x06c0, 0x06c0, 0x0100, 0x0100, 0x0100, 0x0100,
		0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
	},
};

/* The vendor's battery thermistor table, every 5 C of its 1 C steps. */
static const struct board_therm therm[] = {
	{ -300, 1627 }, { -200, 1507 }, { -100, 1345 }, { 0, 1159 }, { 50, 1064 }, { 100, 972 }, { 150, 885 },
	{ 200, 807 }, { 250, 737 }, { 300, 675 }, { 350, 623 }, { 400, 578 }, { 450, 540 }, { 500, 509 },
	{ 550, 482 }, { 600, 461 }, { 650, 442 }, { 700, 427 }, { 750, 415 }, { 790, 407 },
};

/* The vendor's on-commands up to sleep out, after each reset: page
 * registers, then TE on. */
static const struct dcs_step panel_on[] = {
	{ 0xff, 1, 0, { 0x10 } }, { 0xb3, 1, 0, { 0x00 } }, { 0xbb, 1, 0, { 0x10 } },
	{ 0xff, 1, 0, { 0x24 } }, { 0xfb, 1, 0, { 0x01 } }, { 0x66, 1, 0, { 0xd5 } }, { 0x6e, 1, 0, { 0x00 } },
	{ 0x99, 1, 0, { 0xd8 } }, { 0xff, 1, 0, { 0xe0 } }, { 0xfb, 1, 0, { 0x01 } }, { 0x01, 1, 0, { 0xaa } },
	{ 0xff, 1, 0, { 0x10 } }, { 0x35, 1, 0, { 0x00 } }, { 0x11, 0, 100, { 0 } },
};
/* The vendor's off-commands before display off. */
static const struct dcs_step panel_off[] = {
	{ 0xff, 1, 0, { 0x20 } }, { 0xfb, 1, 0, { 0x01 } }, { 0x61, 1, 0, { 0x00 } }, { 0x62, 1, 0, { 0x00 } },
	{ 0xff, 1, 0, { 0x10 } },
};

const struct board board_narwhal = {
	.name = "narwhal",
	.compatible = "lge,narwhal",
	/* Sharp NT35350 360x360 command-mode LCD, TE into the MDP. */
	.panel = {
		.width = 360, .height = 360,
		.hz = 60, .te_gpio = 24, .round = true, .lcd = true, .lp_commands = true,
		/* Switched off with the screen as the vendor does: its enable
		 * line on gpio 49, the LM36272's bias, then reset. Its I/O
		 * supply is L4 or L6 by board revision and stays on. */
		.on = panel_on, .on_n = sizeof panel_on / sizeof panel_on[0],
		.sleep = panel_off, .sleep_n = sizeof panel_off / sizeof panel_off[0],
		.reset_gpio = 25, .bias_gpio = 49,
	},
	.firmware_dirs = firmware_dirs,
	.firmware_part = "vendor",
	/* BCM43430A1 on BLSP1 UART2 as on catfish, REG_ON on gpio 44, host
	 * wake on gpio 48 (MPM 51), device wake on gpio 47, both active low.
	 * LG's bootloader puts the factory address in the tree it passes. */
	.bt = {
		.uart = 2, .gpio = 20, .func = 3, .dev_wake = 47, .host_wake = 48, .reg_on = 44, .mpm = 51,
		.dev_wake_active = false, .host_wake_active = false, .addr_chosen = true,
		.patch = "BCM43430A1.hcd",
		.sleep_mode = { 1, 0x18, 0x18, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
	},
	.pmic_mpm = 62,
	.crash_to_fastboot = true,
	/* The top button is gpio 90 and the bottom one gpio 91. */
	.keys = { { .gpio = 90, .mpm = 30 }, { .gpio = 91, .mpm = 32 } },
	.usb_phy_init = { 0x44, 0x6f, 0x38, 0x33 },
	/* maXTouch with a T100 on BLSP1 QUP5, its core fed by L10 at 3.3 V.
	 * It keeps its own settings for awake. */
	.touch = &touch_mxt,
	.touch_pins = { .i2c = { .bus = 5, .sda = 18, .scl = 19, .func = 2, .addr = 0x4a }, .irq = 13, .reset = 12, .mpm = 10,
			.ldo = 10, .ldo_uv = 3300000, .own_settings = true },
	/* LM36272 on BLSP1 QUP4. */
	.backlight = &backlight_lm36272,
	.backlight_pins = { .i2c = { .bus = 4, .sda = 14, .scl = 15, .func = 2, .addr = 0x11 }, .map = backlight_map,
			    .bias_p = 95, .bias_n = 83 },
	/* SOP716 on BLSP1 QUP4 too, reset on gpio 33. */
	.hands = &hands_sop716,
	.hands_pins = { .i2c = { .bus = 4, .sda = 14, .scl = 15, .func = 2, .addr = 0x48 }, .reset = 33, .bslen = 16,
			.temp = 22 },
	/* PAJ9124U1 on BLSP1 QUP6 (gpio 8, 9, 11, function 1), chip select
	 * gpio 10, motion line gpio 39; its logic on L16, its LED on L17. */
	.crown = &crown_paj9124,
	.crown_pins = { .spi = 6, .mosi = 8, .miso = 9, .clk = 11, .func = 1, .cs = 10, .irq = 39, .mpm = 27,
			.vdd_ldo = 16, .vdd_uv = 1800000, .vld_ldo = 17, .vld_uv = 3300000 },
	/* No hub: an ICM-40604 on BLSP1 QUP3 as SPI (gpio 0-3, function 1, chip
	 * select gpio 2) as on swift, INT1 on gpio 96, from the vendor's sensor
	 * registry. */
	.hub = &hub_icm40604,
	.hub_pins = { .spi = 3, .gpio = 0, .func = 1, .cs = 2, .irq = 96, .mpm = 36 },
	.motor = &motor_pm8226,
	.motor_pins = { .vib_mv = 2900 },
	/* FAN54511 and MAX17050 on BLSP1 QUP4, the charger's interrupt on
	 * gpio 92 (MPM 33) and its disable line on gpio 84. The PM8916's own
	 * charger is off in the vendor's tree; its USB input is suspended. */
	.battery_driver = &battery_fan54511,
	.charger = { .i2c = { .bus = 4, .sda = 14, .scl = 15, .func = 2, .addr = 0x6b }, .irq = 92, .mpm = 33,
		     .setup = charger_setup, .setup_n = sizeof charger_setup / sizeof charger_setup[0],
		     .cell_mah = 240, .hold_low = 84, .pmic_charger_off = true },
	.fuel_gauge = &gauge_max17050,
	.gauge = { .i2c = { .bus = 4, .sda = 14, .scl = 15, .func = 2, .addr = 0x36 }, .model = &cell_model,
		   .soc_full = 95, .soc_empty = 1, .host_temp = true },
	.rsense_uohm = 10000,
	.therm = therm,
	.therm_n = sizeof therm / sizeof therm[0],
};
