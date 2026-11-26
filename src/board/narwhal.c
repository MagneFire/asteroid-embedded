/* LG Watch W7 (narwhal): APQ8009 with a PM8916, the rev-10, rev-c and
 * rev-a boards. Two mechanical hands sit over the LCD. */
#include "asteroid.h"
#include "display/display.h"

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

const struct board board_narwhal = {
	.name = "narwhal",
	.compatible = "lge,narwhal",
	/* Sharp NT35350 360x360 command-mode LCD, TE into the MDP. */
	.panel = {
		.width = 360, .height = 360,
		.hz = 60, .te_gpio = 24, .round = true, .lcd = true, .reset_gpio = 25,
	},
	.firmware_dirs = firmware_dirs,
	.firmware_part = "vendor",
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
	.backlight_pins = { .i2c = { .bus = 4, .sda = 14, .scl = 15, .func = 2, .addr = 0x11 }, .map = backlight_map },
	/* SOP716 on BLSP1 QUP4 too, reset on gpio 33. */
	.hands = &hands_sop716,
	.hands_pins = { .i2c = { .bus = 4, .sda = 14, .scl = 15, .func = 2, .addr = 0x48 }, .reset = 33, .bslen = 16,
			.temp = 22 },
	.motor = &motor_pm8226,
	.motor_pins = { .vib_mv = 2900 },
};
