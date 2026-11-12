#!/bin/sh
# Builds the Cortex-M UI for the host and draws its screens with host.c: PNG files and the heap's
# peak per screen in OUT_DIR (build/ui-mcu/screens by default). Its 64-bit pointers take more
# heap than the watch: run-emu.sh measures that.
# usage: tools/ui-mcu/run-host.sh [HEAP_BYTES [OUT_DIR]]
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
cc -O1 -g -Isrc/soc/qcom -Isrc -o build/ui-mcu/host tools/ui-mcu/host.c "ui/target-mcu-host/$host/mcu/libasteroid_ui.a" -lz
build/ui-mcu/host "$heap" "$out"
