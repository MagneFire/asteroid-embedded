/* Huawei Watch (sturgeon): what differs from watch to watch. Pins are
 * TLMM gpio numbers; MPM pins are the MPM's wake-up sources. */
#ifndef BOARD_H
#define BOARD_H

#define BOARD_NAME		"sturgeon"
#define BOARD_UI_SIZE		400

/* RM69080 400x400 command-mode panel: 434 lines with the porches at
 * 60 Hz, TE into the MDP. */
#define BOARD_PANEL_LINES	434
#define BOARD_PANEL_HZ		60
#define BOARD_PANEL_TE_GPIO	24

/* Synaptics RMI4 touch controller on BLSP1 QUP5. */
#define BOARD_TOUCH_QUP		5
#define BOARD_TOUCH_SDA		18
#define BOARD_TOUCH_SCL		19
#define BOARD_TOUCH_ADDR	0x20
#define BOARD_TOUCH_ATTN	17
#define BOARD_TOUCH_RESET	16
#define BOARD_TOUCH_MPM		8

/* DRV2605 haptics driver on BLSP1 QUP2. */
#define BOARD_HAPTICS_QUP	2
#define BOARD_HAPTICS_SDA	6
#define BOARD_HAPTICS_SCL	7
#define BOARD_HAPTICS_ADDR	0x5a
#define BOARD_HAPTICS_TRIGGER	59
#define BOARD_HAPTICS_ENABLE	60

/* TFA9895 speaker amplifier on BLSP1 QUP3. */
#define BOARD_SPEAKER_QUP	3
#define BOARD_SPEAKER_SDA	10
#define BOARD_SPEAKER_SCL	11
#define BOARD_SPEAKER_ADDR	0x34

/* BCM43430A0 Bluetooth on BLSP1 UART4 (gpio 12-15, function 2). */
#define BOARD_BT_UART		4
#define BOARD_BT_GPIO		12
#define BOARD_BT_FUNC		2
#define BOARD_BT_DEV_WAKE	63
#define BOARD_BT_HOST_WAKE	64
#define BOARD_BT_REG_ON		67
#define BOARD_BT_MPM		28
#define BOARD_BT_PATCH		"4343A0.hcd"

/* STM32 sensor hub on BLSP1 UART1 (gpio 0-3, function 2). */
#define BOARD_HUB_UART		1
#define BOARD_HUB_GPIO		0
#define BOARD_HUB_FUNC		2
#define BOARD_HUB_WAKE_MCU	62
#define BOARD_HUB_MCU_WAKE	65
#define BOARD_HUB_RESET		87
#define BOARD_HUB_BOOT1		106
#define BOARD_HUB_AP_STATUS	108
#define BOARD_HUB_BOOT0		111
#define BOARD_HUB_MCU_STATUS	114
#define BOARD_HUB_MPM		29

/* The PMIC's interrupt line (power key, charger) as an MPM pin. */
#define BOARD_PMIC_MPM		62
#endif
