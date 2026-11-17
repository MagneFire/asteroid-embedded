#!/bin/sh
# Builds host.c for the Cortex-M4 with the firmware's UI library (make SOC=apollo3 builds it) and
# runs it under Unicorn: the heap's peak per screen on the watch's own 32-bit build, and its
# screens as PNG files in OUT_DIR (build/ui-mcu/emu by default).
# PYTHON is a python3 with unicorn and pyelftools, and capstone for PROFILE=SCREEN,... (see emu.py),
# which counts the flash cache at the firmware's addresses if build/apollo3/asteroid.elf is there.
# usage: tools/ui-mcu/run-emu.sh [HEAP_BYTES [OUT_DIR [WIDTHxHEIGHT]]]
set -e
cd "$(dirname "$0")/../.."
heap=${1:-131072}
out=${2:-build/ui-mcu/emu}
size=${3:-402x476}
python=${PYTHON:-python3}
lib=ui/target-apollo3/thumbv7em-none-eabihf/mcu/libasteroid_ui.a
mkdir -p build/ui-mcu
cflags="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -Os -g -ffreestanding -nostdlib -Isrc/soc/apollo3 -Isrc \
	-Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections"
# Its own W and H are the stack's names.
arm-none-eabi-gcc $cflags -c src/ui/stack.c -o build/ui-mcu/stack.o
arm-none-eabi-gcc $cflags -DEMU -DW="${size%x*}" -DH="${size#*x}" \
	-Wl,--gc-sections -Wl,--no-warn-rwx-segments -Wl,-z,noexecstack -Wl,-T,tools/ui-mcu/emu.ld \
	tools/ui-mcu/host.c build/ui-mcu/stack.o "$lib" -lgcc -o build/ui-mcu/emu.elf
"$python" tools/ui-mcu/emu.py build/ui-mcu/emu.elf "$heap" "$out" "$size" ${PROFILE:+--profile "$PROFILE"} \
	$([ -n "$PROFILE" ] && [ -f build/apollo3/asteroid.elf ] && echo --layout build/apollo3/asteroid.elf)
