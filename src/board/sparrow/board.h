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
/* Idle mode: the plain commands, 100 ms to settle. */
#define BOARD_PANEL_IDLE_PREP	0
#define BOARD_PANEL_IDLE_SETTLE_MS 100

/* Firmware on the system partition: the GPU microcode under vendor,
 * the Bluetooth patch and the sensor hub firmware under etc. */
#define BOARD_FIRMWARE_DIRS	"/vendor/firmware/", "/etc/firmware/bt/", "/etc/firmware/"

/* BCM4343A1 Bluetooth on BLSP1 UART1 (gpio 0-3, function 2), REG_ON on
 * gpio 34, host wake on gpio 48 (MPM pin 20), device wake on gpio 61. */
#define BOARD_BT_UART		1
#define BOARD_BT_GPIO		0
#define BOARD_BT_FUNC		2
#define BOARD_BT_DEV_WAKE	61
#define BOARD_BT_HOST_WAKE	48
#define BOARD_BT_REG_ON		34
#define BOARD_BT_MPM		20
#define BOARD_BT_PATCH		"BCM4343A1_001.002.009.0053.0240.hcd"
/* Both wake lines are active high. Sleep mode parameters (0xFC27): mode 1,
 * host and controller idle thresholds 1, dev-wake and host-wake
 * polarity 1 (active high), then the SCO/LPM/tristate flags. */
#define BOARD_BT_DEV_WAKE_ACTIVE	1
#define BOARD_BT_HOST_WAKE_ACTIVE	1
#define BOARD_BT_SLEEP_MODE	{ 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 }

/* ITE IT7260 touch controller on BLSP1 QUP5, interrupt on gpio 17 (MPM
 * pin 8), reset on gpio 16. */
#define BOARD_TOUCH_QUP		5
#define BOARD_TOUCH_SDA		18
#define BOARD_TOUCH_SCL		19
#define BOARD_TOUCH_ADDR	0x46
#define BOARD_TOUCH_IRQ		17
#define BOARD_TOUCH_RESET	16
#define BOARD_TOUCH_MPM		8

/* PNI EM7184 sensor hub on BLSP1 QUP2 (gpio 6/7, function 3), interrupt
 * on gpio 111 (MPM pin 54); its firmware is uploaded at every boot. */
#define BOARD_HUB_QUP		2
#define BOARD_HUB_SDA		6
#define BOARD_HUB_SCL		7
#define BOARD_HUB_FUNC		3
#define BOARD_HUB_ADDR		0x28
#define BOARD_HUB_IRQ		111
#define BOARD_HUB_MPM		54

/* The PMIC's interrupt line (power key, charger) as an MPM pin. */
#define BOARD_PMIC_MPM		62

/* PM8226 vibrator drive voltage. */
#define BOARD_VIB_MV		3000
#endif
