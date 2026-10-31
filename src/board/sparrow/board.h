/* ASUS ZenWatch 2 (sparrow): a scaffold from the downstream device
 * tree, not yet run on the watch. The touch controller (ITE IT7260 at
 * 0x46 on QUP5, irq gpio 17, reset 16), the PNI EM7184 sensor hub (QUP2
 * at 0x28, irq gpio 111), the PM8226 vibrator and the gpio-keys power
 * key (gpio 37) have no drivers yet, so board.mk lists none and the
 * stubs in board/stubs.c stand in. Values marked unverified are the
 * mainline device tree's and need checking on the bench. */
#ifndef BOARD_H
#define BOARD_H

#define BOARD_NAME		"sparrow"
#define BOARD_UI_SIZE		320

/* RM69032 320x320 command-mode panel: 326 lines with the porches at
 * 57 Hz, TE on gpio 24. */
#define BOARD_PANEL_LINES	326
#define BOARD_PANEL_HZ		57
#define BOARD_PANEL_TE_GPIO	24

/* BCM4343A1 Bluetooth on BLSP1 UART1 (gpio 0-3, function 2); the wake
 * pins are unverified, the MPM pin not yet derived. The patch is in
 * /system/etc/firmware/bt on this watch. */
#define BOARD_BT_UART		1
#define BOARD_BT_GPIO		0
#define BOARD_BT_FUNC		2
#define BOARD_BT_DEV_WAKE	61
#define BOARD_BT_HOST_WAKE	48
#define BOARD_BT_REG_ON		34
#define BOARD_BT_MPM		28
#define BOARD_BT_PATCH		"BCM4343A1_001.002.009.0053.0240.hcd"

/* MPM pins: the touch one is unverified (gpio 17 here as on sturgeon),
 * the PMIC one is the chip's. */
#define BOARD_TOUCH_MPM		8
#define BOARD_PMIC_MPM		62
#endif
