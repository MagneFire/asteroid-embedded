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

    make                # build/asteroid.img, for every supported watch

One image runs on both watches. The boot image holds a device tree per
watch, each bootloader passes the one with its board-id, and the firmware
picks the watch from that tree's `compatible`. What differs between the
watches (pins, buses, panel timing, the battery curve, the Bluetooth
patch, which drivers it has) is a table per watch in `src/board/<watch>.c`.
The UI is laid out in fractions of the screen and draws its icons from
outlines at the size it shows them, so it fits any screen size.
    make TEST=hang      # a build that stalls its main loop (watchdog test)
    make TEST=abort     # a build that takes a data abort

The image carries no vendor firmware. The Adreno A3xx microcode
(`a330_pm4.fw`, `a330_pfp.fw`), the Bluetooth patch and, on sparrow, the
sensor hub firmware are read at boot from the watch's own Android system
partition (the directories are listed in each watch's table).

## Run

The watch must sit in fastboot. Sturgeon has lk2nd (built with
`LK2ND_FORCE_FASTBOOT=1` and `SCRATCH_SIZE=0x0F000000`) flashed to `boot`,
which also serves the log region after a crash; sparrow's stock bootloader
boots the image as it is, and `adb reboot bootloader` brings it back.
Smelt has the image flashed to `boot` (AsteroidOS backup in the notes repo),
so after a crash it reboots into the firmware by itself and `--attach` shows
the record; `--serial` and `--adb-serial` pick the watch when several are
attached (the fastboot and the firmware's adb serial differ).

    make run            # adb reboot bootloader, fastboot boot, show the log
    tools/run.py --attach --adb-serial asteroid-smelt   # log and record of a running watch
    make test           # host tests for the adb and console code

The running firmware is an adb device: `adb shell` gives the command
console (`help` lists the commands), `adb push`/`adb pull` move files to
and from a RAM file table, `adb reboot bootloader` returns to fastboot. The firmware
keeps its log and a fault record in DRAM across a warm reset; when an image
dies, `tools/run.py` fetches them through `fastboot oem ramoops raw` and
symbolizes the addresses in the fault record with `addr2line`.

## License

GPL-3.0-or-later (see LICENSE). Files taken from other projects keep their
own license: TinyUSB and its ChipIdea port (MIT), the 5x12 console font
(BSD, from the Android Open Source Project), the FlatMesh geometry
(BSD-3-Clause, from qml-asteroid), the Noto Sans font in `ui/fonts/`
(OFL-1.1) and the Ionicons in `ui/icons/` (MIT, from asteroid-icons-ion,
resized). Slint is used under its GPL-3.0-only option.
