#!/bin/sh
# Builds the Cortex-M UI for the host and draws its screens with host.c: PNG files and the heap's
# peak per screen in OUT_DIR (build/ui-mcu/screens by default). Its 64-bit pointers take more
# heap than the watch: run-emu.sh measures that. SIZE=WxH draws another screen size than the
# 46 mm watch's 402x476.
# usage: [SIZE=WxH] tools/ui-mcu/run-host.sh [HEAP_BYTES [OUT_DIR]]
set -e
cd "$(dirname "$0")/../.."
heap=${1:-262144}
out=${2:-build/ui-mcu/screens}
cargo=${CARGO:-$(command -v cargo || echo /opt/homebrew/opt/rustup/bin/cargo)}
host=$(rustc -vV 2>/dev/null | sed -n 's/^host: //p')
host=${host:-aarch64-apple-darwin}
(cd ui && PATH="$(dirname "$cargo"):$PATH" "$cargo" build --profile mcu --target "$host" --no-default-features --features mcu \
	--target-dir "$PWD/target-mcu-host")
mkdir -p "$out"
size=
[ -z "$SIZE" ] || size="-DW=${SIZE%x*} -DH=${SIZE#*x}"
mkdir -p build/ui-mcu
# Only host.c: W and H are names of stack.c's own.
cc -O1 -g $size -Isrc/soc/qcom -Isrc -c -o build/ui-mcu/host.o tools/ui-mcu/host.c
cc -O1 -g -Isrc/soc/qcom -Isrc -o build/ui-mcu/host build/ui-mcu/host.o src/ui/stack.c src/display/flatmesh_mcu.c "ui/target-mcu-host/$host/mcu/libasteroid_ui.a" -lz
build/ui-mcu/host "$heap" "$out"
