# The drivers only this watch has; the rest is common.
BOARD_SRCS := src/input/touch.c src/input/haptics.c src/input/haptics_drv2605.c src/sensors/hub.c src/audio/speaker.c
BOARD_UI_SIZE := 400
# The sensor hub reads the heart rate.
BOARD_HEART_RATE := 1
