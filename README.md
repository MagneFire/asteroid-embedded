# asteroid-embedded

Bare-metal firmware for Qualcomm APQ8026 smartwatches (Huawei Watch
"sturgeon", ASUS ZenWatch 2 "sparrow"): no Linux kernel, no root filesystem,
one image booted over fastboot. The bootloader leaves the panel on and the
chip configured; the firmware takes it from there.

## Build

Needs `arm-none-eabi-gcc`, `dtc`, Python 3 and Rust (`cargo` on PATH or
from Homebrew's rustup, with `rustup target add armv7a-none-eabihf`). The
user interface in `ui/` is a Slint crate built into a static library and
linked into the image.

    make                # build/asteroid.img for sturgeon
    make WATCH=sparrow
    make TEST=hang      # a build that stalls its main loop (watchdog test)
    make TEST=abort     # a build that takes a data abort

Every `firmware/*.fw` file goes into the boot image's ramdisk, which the
bootloader loads next to the firmware; the firmware finds the files there
by name. `firmware/` holds the Adreno A3xx microcode (`a330_pm4.fw`,
`a330_pfp.fw`, identical to linux-firmware's `qcom/` copies).

## Run

The watch must sit in fastboot (lk2nd built with `LK2ND_FORCE_FASTBOOT=1`
and `SCRATCH_SIZE=0x0F000000` flashed to `boot`).

    make run            # adb reboot, fastboot boot, show the log over adb
    make test           # host tests for the adb and console code

The running firmware is an adb device: `adb shell` gives the command
console (`help` lists the commands), `adb push`/`adb pull` move files to
and from a RAM file table, `adb reboot` returns to fastboot. The firmware
keeps its log and a fault record in DRAM across a warm reset; when an image
dies, `tools/run.py` fetches them through `fastboot oem ramoops raw` and
symbolizes the addresses in the fault record with `addr2line`.

## License

GPL-3.0-or-later (see LICENSE). Files taken from other projects keep their
own license: TinyUSB and its ChipIdea port (MIT), the 5x12 console font
(BSD, from the Android Open Source Project), the FlatMesh geometry
(BSD-3-Clause, from qml-asteroid) and the Noto Sans font in `ui/fonts/`
(OFL-1.1). Slint is used under its GPL-3.0-only option. The GPU microcode
in `firmware/` is Qualcomm's, redistributable under `firmware/LICENSE.qcom`;
it is loaded from the boot image's ramdisk and is not part of the firmware
binary.
