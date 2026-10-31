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
#define BOARD_PANEL_FLIP_UD	0

/* Firmware on the system partition: the GPU microcode under vendor,
 * the Bluetooth patch and the sensor hub firmware under etc. */
#define BOARD_FIRMWARE_DIRS	"/vendor/firmware/", "/etc/firmware/bt/", "/etc/firmware/"

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

/* ITE IT7260 touch controller on BLSP1 QUP5, interrupt on gpio 17 (MPM
 * pin 8), reset on gpio 16. */
#define BOARD_TOUCH_QUP		5
#define BOARD_TOUCH_SDA		18
#define BOARD_TOUCH_SCL		19
#define BOARD_TOUCH_ADDR	0x46
#define BOARD_TOUCH_IRQ		17
#define BOARD_TOUCH_RESET	16
#define BOARD_TOUCH_MPM		8

/* The PMIC's interrupt line (power key, charger) as an MPM pin. */
#define BOARD_PMIC_MPM		62
#endif
