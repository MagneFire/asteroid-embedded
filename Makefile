# asteroid-embedded: bare-metal firmware for smartwatches.
# make            build/asteroid.img for the APQ8026 watches and
#                 build/omap3/asteroid.img for the OMAP3630 one, side by side
# make SOC=qcom   only the APQ8026 image (SOC=omap3 only minnow's)
# make UI_FAST=1  a UI built in half the time (thin LTO), larger and slower:
#                 not for measuring speed or power
# make JOBS=1     one job at a time (default: one per CPU; Make 3.81 ignores
#                 a -j given on the command line)
# make run        boot it over fastboot, wait for the reboot, show the log
# make test       host unit tests of the console, adb and the parsers
# make textbench  time Agenda-like text repaints on the host, with the UI's Slint
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
DTC     ?= dtc
# The tools carry their Python version and packages for uv (PEP 723).
PYTHON  ?= $(if $(shell command -v uv),uv run --script,python3)
TEST    ?=

JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc)
# Only the top make: a sub-make given its own -j leaves the jobserver.
# Not with clean, which would run alongside the build.
ifeq ($(MAKELEVEL)$(filter clean,$(MAKECMDGOALS)),0)
MAKEFLAGS += -j$(or $(JOBS),1)
endif

# The SoC the image is for: qcom (APQ8026) or omap3 (OMAP3630). Its code
# is in src/soc/$(SOC), and each has its own image.
SOC    ?= qcom
BUILD  := build$(if $(filter qcom,$(SOC)),,/$(SOC))
TUSB   := third_party/tinyusb/src
NIMBLE := third_party/nimble
MBEDTLS := third_party/mbedtls
GITREV := $(shell git rev-parse --short=10 HEAD 2>/dev/null || echo nogit)
DIRTY  := $(shell git diff --quiet HEAD 2>/dev/null || echo +)

# What every watch runs: the services, the UI's glue and the Bluetooth
# host, with no assumption about the processor or the storage.
SRCS := src/main.c src/version.c src/prof.c src/settings.c src/alarm.c src/clockapps.c src/calc.c src/game.c src/agenda.c src/fitness.c src/workout.c src/hrlog.c \
        src/power.c src/power_policy.c src/sched.c src/stats.c src/app.c src/actions.c src/events.c src/present.c \
        src/civil.c src/log.c src/lib/ramoops.c src/board/drivers.c \
        src/sensors/steps.c src/sensors/auto_brightness.c src/sensors/orient.c src/sensors/tsl2584.c src/input/haptics.c \
        src/soc/charge_policy.c src/audio/speaker.c \
        src/lib/libc.c src/lib/fmt.c src/lib/fdt.c src/lib/crc32.c src/ui/ui.c src/ui/stack.c src/ui/rotation.c \
        src/bt/npl.c src/bt/transport.c src/bt/store.c src/bt/gatt.c src/bt/host.c src/console/console.c \
        src/notify/notify.c src/media/media.c src/weather/weather.c src/storage/kv.c

# The application processors: MMU, eMMC, USB, a frame buffer, and HCI
# over a UART.
SRCS_ap := src/start.S src/soc/cache.S src/frame.c src/soc/mmu.c src/soc/exceptions.c \
        src/soc/max17050.c src/soc/gauge_max17050.c \
        src/sensors/hub_m4.c src/sensors/m4_proto.c src/input/mxt.c src/input/touch_mxt.c src/input/haptics_ldo.c \
        src/display/backlight_lm3535.c src/display/fbcon.c src/display/flatmesh.c \
        src/bt/hci.c src/lib/inflate.c src/usb/usb.c src/usb/adb.c src/usb/ramfs.c src/screenshot/screenshot.c src/screenshot/jpeg.c \
        src/storage/emmc.c src/storage/ext4.c src/storage/rootfs.c src/storage/firmware.c src/storage/squashfs.c src/lib/lz4.c src/storage/store.c \
        $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c \
        $(TUSB)/class/cdc/cdc_device.c

SRCS_qcom := $(SRCS_ap) $(addprefix src/soc/qcom/,init.c chip.c mmu.c gic.c timer.c wdt.c spmi.c pmic.c bms.c reboot.c scm.c tlmm.c clock.c spm.c \
               suspend.S i2c_qup.c spi_qup.c uart_dm.c rpm.c smem_find.c mdt.c smd.c wcnss.c wcnss_msg.c) \
        src/board/detect.c src/board/sturgeon.c src/board/sparrow.c src/board/smelt.c src/board/catfish.c src/board/dory.c src/board/lenok.c src/board/swift.c \
        src/sensors/hub.c src/sensors/hub_em7184.c src/sensors/hub_lis3dsh.c src/sensors/hub_lsm6ds3.c src/sensors/compass.c src/sensors/fusion.c src/compass_app.c src/sensors/hub_nanohub.c src/sensors/hr_accept.c src/sensors/nanohub_proto.c src/sensors/light_tsl2584.c src/sensors/light_al3320.c \
        src/input/touch.c src/input/touch_it7260.c src/input/fts.c src/input/touch_fts.c src/input/hx852x.c src/input/touch_hx852x.c src/input/haptics_drv2605.c src/input/haptics_pmic.c src/input/haptics_gp.c src/display/backlight_lm3530.c src/soc/qcom/battery_pmic_gauge.c \
        src/display/display.c src/display/mdp3.c src/display/dsi.c src/soc/charger_smb231.c src/soc/stc3117.c src/soc/gauge_stc3117.c src/soc/bq27421.c src/soc/gauge_bq27421.c src/gpu/a3xx.c src/gpu/draw.c src/gpu/paint.c src/gpu/soft.c src/gpu/qcom_compose.c \
        src/bt/bcm.c src/usb/dcd_ci_hs_msm.c src/usb/port_ci_hs_msm.c src/storage/emmc_msm.c
CPU_qcom  := cortex-a7
# Only the minnow build sets glyph_bins.
UI_CFG_qcom := --check-cfg cfg(glyph_bins)
FPU_qcom  := neon-vfpv4
TUSB_PORT_qcom := chipidea/ci_hs
TUSB_DEFS_qcom := -DTUP_USBIP_CHIPIDEA_HS -DTUP_DCD_ENDPOINT_MAX=16
DEFS_qcom := -DHAVE_COMPASS

SRCS_omap3 := $(SRCS_ap) $(addprefix src/soc/omap3/,init.c intc.c timer.c wdt.c clock.c gpio.c i2c.c tps65912.c reboot.c sleep.c \
               detect.c rtc.c hsmmc.c dss.c uart.c) \
        src/board/minnow.c src/gpu/soft.c src/gpu/sgx.c src/gpu/sgx_image.c src/gpu/sgx_draw.c src/gpu/sgx_scene_data.c src/gpu/sgx_compose.c src/gpu/sgx_paint.c src/display/display_minnow.c src/usb/dcd_musb_omap3.c src/usb/port_musb_omap3.c src/bt/ti.c
CPU_omap3 := cortex-a8
# Counts what the UI paints, for the paint line in the log, and lets
# the console set the glyph positions per pixel.
UI_CFG_omap3 := --cfg paint_counts --cfg glyph_bins --check-cfg cfg(glyph_bins)
FPU_omap3 := neon
TUSB_PORT_omap3 := mentor/musb
# The Mentor driver keeps its endpoint interrupts in 8 bits.
TUSB_DEFS_omap3 := -DTUP_USBIP_MUSB -DTUP_USBIP_MUSB_TI -DTUP_DCD_ENDPOINT_MAX=8

# The OPPO Watch's Apollo3 Blue microcontroller: no USB, eMMC or frame
# buffer; the store is in its flash and the console goes over SWD.
SRCS_apollo3 := $(addprefix src/soc/apollo3/,start.S nvic.c fault.c init.c timer.c wdt.c gpio.c iom.c sleep.c reboot.c \
               flash.c adc.c detect.c swdbox.c swdwake.c burst.c power_mcu.c absent.c ios.c) \
        src/frame_mcu.c src/display/flatmesh_mcu.c src/board/beluga.c src/display/display_rm69090.c src/display/rm69090_id.c src/input/touch_tma525.c src/input/tma525_proto.c \
        src/input/haptics_aw8624.c src/sensors/hub_lsm6dso.c src/sensors/afe4900.c src/sensors/hr.c src/sensors/raise.c src/sensors/compass.c src/sensors/fusion.c src/sensors/ak09918.c \
        src/sensors/light_tsl258x.c src/sensors/tsl258x.c src/storage/store_flash.c src/storage/none.c \
        src/bt/bleif.c src/bt/bleif_hci.c src/bt/bleif_proto.c src/bt/apollo3_ble_patch.c \
        src/companion/wire.c src/companion/link.c src/companion/proto.c src/companion/companion.c src/companion/sensors.c \
        src/compass_app.c
# Only beluga has a magnetometer.
DEFS_apollo3 := -DHAVE_COMPASS
CPU_apollo3 := cortex-m4
FPU_apollo3 := fpv4-sp-d16
ISA_apollo3 := -mthumb
LDS_apollo3 := ld/asteroid-m.ld.in
RUST_TARGET_apollo3 := thumbv7em-none-eabihf
UI_FEATURES_apollo3 := mcu
UI_PROFILE_apollo3 := mcu
# Only the minnow build sets glyph_bins.
UI_CFG_apollo3 := --check-cfg cfg(glyph_bins)
# For tools/ui-mcu/size.py.
LDFLAGS_apollo3 := -Wl,-Map,$(BUILD)/asteroid.map
# Panic locations name their files: keep the paths short.
UI_RUSTFLAGS_apollo3 := --remap-path-prefix=$(CURDIR)/= --remap-path-prefix=$(HOME)/.cargo/registry/src/=

SRCS += $(SRCS_$(SOC))
NIMBLE_SRCS := $(filter-out %/ble_gatts_lcl.c,$(wildcard $(NIMBLE)/nimble/host/src/*.c)) \
        $(NIMBLE)/nimble/host/services/gap/src/ble_svc_gap.c $(NIMBLE)/nimble/host/services/gatt/src/ble_svc_gatt.c \
        $(NIMBLE)/nimble/transport/src/transport.c \
        $(addprefix $(NIMBLE)/porting/nimble/src/,endian.c mem.c nimble_port.c os_mbuf.c os_mempool.c os_msys.c)
MBEDTLS_SRCS := $(addprefix $(MBEDTLS)/library/,aes.c bignum.c bignum_core.c cipher.c cipher_wrap.c cmac.c \
        constant_time.c ecdh.c ecp.c ecp_curves.c memory_buffer_alloc.c platform.c platform_util.c)
SRCS += $(NIMBLE_SRCS) $(MBEDTLS_SRCS)
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS))

ISA     := $(or $(ISA_$(SOC)),-marm)
LDS     := $(or $(LDS_$(SOC)),ld/asteroid.ld.in)
RUST_TARGET := $(or $(RUST_TARGET_$(SOC)),armv7a-none-eabihf)
CFLAGS := -mcpu=$(CPU_$(SOC)) $(ISA) -mfloat-abi=hard -mfpu=$(FPU_$(SOC)) -ffreestanding -nostdlib \
          -Os -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -ffunction-sections -fdata-sections -fno-common \
          -Isrc -Isrc/soc/$(SOC) -I$(BUILD) -Isrc/usb -I$(TUSB) -I$(TUSB)/portable/$(TUSB_PORT_$(SOC)) -Isrc/bt/port \
          $(addprefix -isystem $(NIMBLE)/,nimble/include nimble/host/include nimble/host/services/gap/include \
            nimble/host/services/gatt/include nimble/transport/include porting/nimble/include) \
          -isystem $(MBEDTLS)/include -DMBEDTLS_CONFIG_FILE='"bt_mbedtls_config.h"' $(TUSB_DEFS_$(SOC)) $(DEFS_$(SOC))
LDFLAGS := -nostdlib -Wl,--gc-sections -Wl,-T,$(BUILD)/asteroid.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-z,noexecstack
LIBS    := -lgcc

# The UI is built once per CPU: the A8 has neither the A7's divide nor
# its fused multiply-add.
CARGO   ?= $(or $(shell command -v cargo 2>/dev/null),/opt/homebrew/opt/rustup/bin/cargo)
UI_TARGET := ui/target$(if $(filter qcom,$(SOC)),,-$(SOC))
UI_PROFILE := $(or $(UI_PROFILE_$(SOC)),$(if $(UI_FAST),fast,release))
UI_LIB  := $(UI_TARGET)/$(RUST_TARGET)/$(UI_PROFILE)/libasteroid_ui.a
UI_SRCS := ui/Cargo.toml ui/Cargo.lock ui/build.rs ui/.cargo/config.toml \
           $(shell find ui/src ui/ui ui/build ui/fonts ui/icons third_party/slint third_party/asteroid-icons-ion/src 2>/dev/null) \
           src/ui/codes.h

ifeq ($(origin SOC),file)
all: $(BUILD)/asteroid.img omap3
omap3:
	$(MAKE) SOC=omap3 build/omap3/asteroid.img
.PHONY: omap3
else ifeq ($(SOC),apollo3)
all: $(BUILD)/asteroid.bin
else
all: $(BUILD)/asteroid.img
endif

$(BUILD)/gitrev.h: FORCE
	@mkdir -p $(BUILD)
	@echo '#define GITREV "$(GITREV)$(DIRTY)"' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
$(BUILD)/config.h: FORCE
	@mkdir -p $(BUILD)
	@echo '$(if $(TEST),#define TEST_$(shell echo $(TEST) | tr a-z A-Z) 1,)' > $@.tmp
	@echo '$(if $(ASLEEP_STATS),#define ASLEEP_STATS 1,)' >> $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
# Which UI the image has, so that switching UI_FAST links it again. The
# image goes too, as make 3.81 cannot tell apart files of the same second.
$(BUILD)/ui-lib: FORCE
	@mkdir -p $(BUILD)
	@echo '$(UI_LIB)' > $@.tmp
	@cmp -s $@.tmp $@ || { mv $@.tmp $@; rm -f $(BUILD)/asteroid.elf; }
	@rm -f $@.tmp
$(BUILD)/ui-parts: FORCE
	@mkdir -p $(BUILD)
	@echo '$(UI_LEAVE_OUT)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
FORCE:

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@


THIRD_PARTY_CFLAGS ?= -w
$(BUILD)/$(NIMBLE)/%.c.o $(BUILD)/$(MBEDTLS)/%.c.o: CFLAGS += $(THIRD_PARTY_CFLAGS)
# Included from -isystem headers, which -MMD leaves out of the .d files.
$(OBJS): $(wildcard src/bt/port/*.h src/bt/port/*/*.h)

$(BUILD)/src/version.c.o $(BUILD)/src/soc/apollo3/ramtest.c.o: $(BUILD)/gitrev.h
$(BUILD)/src/%.c.o: CFLAGS += -Werror
# Only the files that include config.h depend on it, through their .d files.
$(BUILD)/%.c.o: %.c | $(BUILD)/config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(UI_LIB): $(UI_SRCS) $(BUILD)/ui-parts
	cd ui && PATH="$(dir $(CARGO)):$$PATH" CARGO_TARGET_$(shell echo $(RUST_TARGET) | tr a-z- A-Z_)_RUSTFLAGS="-C target-cpu=$(CPU_$(SOC)) $(UI_CFG_$(SOC)) $(UI_RUSTFLAGS_$(SOC))" \
		$(CARGO) build --profile $(UI_PROFILE) --target $(RUST_TARGET) --target-dir $(abspath $(UI_TARGET)) \
		$(if $(UI_FEATURES_$(SOC)),--no-default-features --features $(UI_FEATURES_$(SOC)))
	@touch $@

$(BUILD)/asteroid.ld: $(LDS) src/soc/$(SOC)/memmap.h
	@mkdir -p $(BUILD)
	$(CC) -E -P -x c -DLINKER_SCRIPT -Isrc/soc/$(SOC) $< -o $@

$(BUILD)/asteroid.elf: $(OBJS) $(UI_LIB) $(BUILD)/ui-lib $(BUILD)/asteroid.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDFLAGS_$(SOC)) $(OBJS) $(LIBS) $(UI_LIB) $(LIBS) -o $@
	$(SIZE) $@

$(BUILD)/asteroid.bin: $(BUILD)/asteroid.elf
	$(OBJCOPY) -O binary $< $@

# The SRAM test image for `swdtool ramrun`.
$(BUILD)/ramtest.ld: ld/asteroid-ram.ld.in src/soc/$(SOC)/memmap.h
	@mkdir -p $(BUILD)
	$(CC) -E -P -x c -DLINKER_SCRIPT -Isrc/soc/$(SOC) $< -o $@
$(BUILD)/ramtest.elf: $(filter-out $(BUILD)/src/main.c.o $(BUILD)/src/soc/apollo3/flash.c.o,$(OBJS)) $(BUILD)/src/soc/apollo3/ramtest.c.o $(UI_LIB) $(BUILD)/ramtest.ld
	$(CC) $(CFLAGS) $(subst asteroid.ld,ramtest.ld,$(LDFLAGS)) $(filter %.o,$^) $(LIBS) $(UI_LIB) $(LIBS) -o $@
	$(SIZE) $@
$(BUILD)/ramtest.bin: $(BUILD)/ramtest.elf
	$(OBJCOPY) -O binary $< $@
ramtest: $(BUILD)/ramtest.bin

$(BUILD)/%.dtb: dts/%.dts
	@mkdir -p $(BUILD)
	$(DTC) -q -a 8 -I dts -O dtb -o $@ $<

ifeq ($(SOC),qcom)
# One tree per watch: each bootloader picks the one whose board-id it has.
# A new tree goes last, so the entries before it stay as they are.
DTBS := $(BUILD)/sparrow.dtb $(BUILD)/sturgeon.dtb $(BUILD)/smelt.dtb $(BUILD)/catfish.dtb $(BUILD)/swift.dtb

# The bootloader of dory and lenok reads entries of its own layout, after
# the others.
$(BUILD)/qcdt.img: $(DTBS) $(BUILD)/dory.dtb $(BUILD)/lenok.dtb tools/qcdt.py
	$(PYTHON) tools/qcdt.py -o $@ $(DTBS) --lge $(BUILD)/dory.dtb --lge $(BUILD)/lenok.dtb

# One image for every watch. Bootloaders that read the table use it, and
# ignore what follows the firmware. smelt's own images have no table and
# the tree behind the kernel, so that is there too, in case its bootloader
# only looks there. catfish's bootloader takes the tree from there and
# dies on smelt's, so catfish's goes first, and swift's, whose bootloader
# is of the same kind, before smelt's too.
$(BUILD)/asteroid-tree.bin: $(BUILD)/asteroid.bin $(BUILD)/catfish.dtb $(BUILD)/swift.dtb $(BUILD)/smelt.dtb tools/appenddtb.py
	$(PYTHON) tools/appenddtb.py -o $@ $< $(BUILD)/catfish.dtb $(BUILD)/swift.dtb $(BUILD)/smelt.dtb

$(BUILD)/asteroid.img: $(BUILD)/asteroid-tree.bin $(BUILD)/qcdt.img tools/mkbootimg.py
	$(PYTHON) tools/mkbootimg.py --kernel $< --dt $(BUILD)/qcdt.img -o $@
else
# The bootloader takes its tree from behind the image, as it does with
# the vendor's kernels, and picks it by the hardware revision the tree
# names. Its own images all have a ramdisk, so this one has one too,
# with nothing in it.
$(BUILD)/asteroid-tree.bin: $(BUILD)/asteroid.bin $(BUILD)/minnow.dtb tools/appenddtb.py
	$(PYTHON) tools/appenddtb.py -o $@ $< $(BUILD)/minnow.dtb

$(BUILD)/ramdisk.bin:
	@mkdir -p $(BUILD)
	head -c 512 /dev/zero > $@

$(BUILD)/asteroid.img: $(BUILD)/asteroid-tree.bin $(BUILD)/ramdisk.bin tools/mkbootimg.py
	$(PYTHON) tools/mkbootimg.py --kernel $< --ramdisk $(BUILD)/ramdisk.bin --cmdline androidboot.hardware=minnow -o $@
endif

run: $(BUILD)/asteroid.img
	$(PYTHON) tools/run.py $(if $(filter qcom,$(SOC)),,--soc $(SOC)) $(RUNFLAGS)

SLINT_VIEWER ?= $(or $(shell command -v slint-viewer 2>/dev/null),$(HOME)/.cargo/bin/slint-viewer)

# The viewer lays the UI out on Theme's default screen; icons are drawn by the firmware, so the
# viewer shows none.
preview: $(UI_LIB)
	$(SLINT_VIEWER) -I $(abspath $(dir $(firstword $(wildcard ui/target/armv7a-none-eabihf/release/build/asteroid-ui-*/out/codes.slint)))) \
		--auto-reload --load-data tools/preview/watchface.json ui/ui/preview.slint

# Its own crate, built for the host, so ui's armv7a target and allocator stay out of it.
textbench:
	cd ui/textbench && PATH="$(dir $(CARGO)):$$PATH" $(CARGO) run --release \
		--target $$(PATH="$(dir $(CARGO)):$$PATH" rustc -vV | sed -n 's/^host: //p') --target-dir $(abspath build/textbench)

disasm: $(BUILD)/asteroid.elf
	$(OBJDUMP) -d $< > $(BUILD)/asteroid.dis

HOSTCC      ?= cc
HOST_CFLAGS := -std=gnu11 -g -O1 -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-format -fno-builtin \
               -U_FORTIFY_SOURCE -I src -I src/soc/qcom -I tests -DHOST_TEST -include tests/host.h \
               -fsanitize=address,undefined -fno-sanitize-recover=all
HOST_SRCS   := tests/stubs.c src/console/console.c src/usb/adb.c src/usb/ramfs.c src/lib/fmt.c src/clockapps.c src/alarm.c src/ui/stack.c src/calc.c src/game.c src/agenda.c src/sensors/orient.c src/ui/rotation.c
PARSER_SRCS := src/notify/notify.c src/weather/weather.c src/media/media.c src/storage/kv.c src/storage/ext4.c

$(BUILD)/test/parser_test: tests/parser_test.c $(PARSER_SRCS) $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< $(PARSER_SRCS) -o $@

$(BUILD)/test/actions_test: tests/actions_test.c src/actions.c src/civil.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) -DSOC_RESET_WATCH $< src/actions.c src/civil.c -o $@

$(BUILD)/test/power_test: tests/power_test.c src/power_policy.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/power_policy.c -o $@

$(BUILD)/test/sched_test: tests/sched_test.c src/sched.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/sched.c -o $@

$(BUILD)/test/events_test: tests/events_test.c src/events.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/events.c -o $@

$(BUILD)/test/steps_test: tests/steps_test.c src/sensors/steps.c src/fitness.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/sensors/steps.c src/fitness.c -o $@

$(BUILD)/test/fitness_test: tests/fitness_test.c src/fitness.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/fitness.c -o $@

$(BUILD)/test/workout_test: tests/workout_test.c src/workout.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/workout.c -o $@

$(BUILD)/test/hr_accept_test: tests/hr_accept_test.c src/sensors/hr_accept.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/sensors/hr_accept.c -o $@

$(BUILD)/test/hrlog_test: tests/hrlog_test.c src/hrlog.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/hrlog.c -o $@

$(BUILD)/test/gauge_test: tests/gauge_test.c src/soc/max17050.c src/soc/charge_policy.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/soc/max17050.c src/soc/charge_policy.c -o $@

$(BUILD)/test/backlight_test: tests/backlight_test.c src/display/backlight_lm3535.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< -o $@

$(BUILD)/test/mxt_test: tests/mxt_test.c src/input/mxt.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/input/mxt.c -o $@

$(BUILD)/test/fts_test: tests/fts_test.c src/input/fts.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/input/fts.c -o $@

$(BUILD)/test/stc3117_test: tests/stc3117_test.c src/soc/stc3117.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/soc/stc3117.c -o $@

$(BUILD)/test/bq27421_test: tests/bq27421_test.c src/soc/bq27421.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/soc/bq27421.c -o $@

$(BUILD)/test/smem_test: tests/smem_test.c src/soc/qcom/smem_find.c $(wildcard tests/*.h src/*.h src/*/*.h src/soc/qcom/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/soc/qcom/smem_find.c -o $@

$(BUILD)/test/mdt_test: tests/mdt_test.c src/soc/qcom/mdt.c $(wildcard tests/*.h src/*.h src/*/*.h src/soc/qcom/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/soc/qcom/mdt.c -o $@

$(BUILD)/test/smd_test: tests/smd_test.c src/soc/qcom/smd.c $(wildcard tests/*.h src/*.h src/*/*.h src/soc/qcom/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/soc/qcom/smd.c -o $@

$(BUILD)/test/wcnss_msg_test: tests/wcnss_msg_test.c src/soc/qcom/wcnss_msg.c $(wildcard tests/*.h src/*.h src/*/*.h src/soc/qcom/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/soc/qcom/wcnss_msg.c -o $@

$(BUILD)/test/hx852x_test: tests/hx852x_test.c src/input/hx852x.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/input/hx852x.c -o $@

$(BUILD)/test/m4_test: tests/m4_test.c src/sensors/m4_proto.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/sensors/m4_proto.c -o $@

$(BUILD)/test/nanohub_test: tests/nanohub_test.c src/sensors/nanohub_proto.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/sensors/nanohub_proto.c -o $@

$(BUILD)/test/al3320_test: tests/al3320_test.c src/sensors/light_al3320.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< -o $@

$(BUILD)/test/tsl2584_test: tests/tsl2584_test.c src/sensors/light_tsl2584.c src/sensors/tsl2584.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/sensors/tsl2584.c -o $@

$(BUILD)/test/tsl258x_test: tests/tsl258x_test.c src/sensors/light_tsl258x.c src/sensors/tsl258x.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/sensors/tsl258x.c -o $@

$(BUILD)/test/auto_brightness_test: tests/auto_brightness_test.c src/sensors/auto_brightness.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/sensors/auto_brightness.c -o $@

$(BUILD)/test/ramoops_test: tests/ramoops_test.c src/lib/ramoops.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/lib/ramoops.c -o $@

$(BUILD)/test/emmc_test: tests/emmc_test.c src/storage/emmc.c src/lib/fmt.c src/lib/crc32.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/storage/emmc.c src/lib/fmt.c src/lib/crc32.c -o $@

$(BUILD)/test/squashfs_test: tests/squashfs_test.c src/storage/squashfs.c src/lib/inflate.c src/lib/lz4.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/storage/squashfs.c src/lib/inflate.c src/lib/lz4.c -o $@

$(BUILD)/test/soft_test: tests/soft_test.c src/gpu/soft.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/gpu/soft.c -o $@

# Timing of the CPU's texture drawing: the soft_test run with SOFT_BENCH=1 on an optimised build without
# the sanitizers. SOFT_BENCH_SRC names another soft.c to time against.
SOFT_BENCH_SRC ?= src/gpu/soft.c
$(BUILD)/test/soft_bench: tests/soft_test.c $(SOFT_BENCH_SRC) $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -g -fsanitize=% -fno-sanitize-recover=all,$(HOST_CFLAGS)) -O2 $< $(SOFT_BENCH_SRC) -o $@

bench-soft: FORCE $(BUILD)/test/soft_bench
	SOFT_BENCH=1 $(BUILD)/test/soft_bench

$(BUILD)/test/sgx_compose_test: tests/sgx_compose_test.c src/gpu/sgx_compose.c src/gpu/soft.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/gpu/sgx_compose.c src/gpu/soft.c -o $@

$(BUILD)/test/qcom_compose_test: tests/qcom_compose_test.c src/gpu/qcom_compose.c src/gpu/soft.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/gpu/soft.c -o $@

$(BUILD)/test/sgx_image_test: tests/sgx_image_test.c src/gpu/sgx_image.c src/gpu/sgx_image.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/gpu/sgx_image.c -o $@

$(BUILD)/test/sgx_draw_test: tests/sgx_draw_test.c src/gpu/sgx_draw.c src/gpu/sgx_scene_data.c src/gpu/sgx_image.c $(wildcard src/gpu/sgx*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/gpu/sgx_draw.c src/gpu/sgx_scene_data.c src/gpu/sgx_image.c -o $@

# sgx.c drives registers, which the test fakes, and needs the OMAP3 map.
SGX_HOST_CFLAGS := $(subst -I src/soc/qcom,-I src/soc/omap3,$(subst tests/host.h,tests/host_mmio.h,$(HOST_CFLAGS)))
$(BUILD)/test/sgx_test: tests/sgx_test.c src/gpu/sgx.c src/gpu/sgx_image.c $(wildcard tests/*.h src/*.h src/gpu/sgx*.h src/soc/omap3/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(SGX_HOST_CFLAGS) $< src/gpu/sgx.c src/gpu/sgx_image.c -o $@

SGX_STACK := src/gpu/sgx_compose.c src/gpu/soft.c src/gpu/sgx_draw.c src/gpu/sgx_scene_data.c src/gpu/sgx_image.c
$(BUILD)/test/sgx_stack_test: tests/sgx_stack_test.c $(SGX_STACK) $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< $(SGX_STACK) -o $@

SGX_PAINT := src/gpu/sgx_paint.c src/gpu/soft.c
$(BUILD)/test/sgx_paint_test: tests/sgx_paint_test.c $(SGX_PAINT) $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< $(SGX_PAINT) -o $@

$(BUILD)/test/board_test: tests/board_test.c src/board/detect.c src/board/sturgeon.c src/board/sparrow.c src/board/smelt.c src/board/catfish.c src/board/dory.c src/board/lenok.c src/board/swift.c src/sensors/m4_proto.c src/lib/fdt.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/board/detect.c src/board/sturgeon.c src/board/sparrow.c src/board/smelt.c src/board/catfish.c src/board/dory.c src/board/lenok.c src/board/swift.c src/sensors/m4_proto.c src/lib/fdt.c -o $@

$(BUILD)/test/bleif_test: tests/bleif_test.c src/bt/bleif_proto.c src/bt/apollo3_ble_patch.c src/bt/bleif_proto.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/bt/bleif_proto.c src/bt/apollo3_ble_patch.c -o $@

COMPANION := src/companion/wire.c src/companion/proto.c src/companion/link.c src/companion/companion.c src/companion/sensors.c src/sensors/compass.c src/sensors/fusion.c
$(BUILD)/test/hr_test: tests/hr_test.c src/sensors/hr.c src/sensors/hr.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/sensors/hr.c -o $@

$(BUILD)/test/raise_test: tests/raise_test.c src/sensors/raise.c src/sensors/raise.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/sensors/raise.c -o $@

$(BUILD)/test/compass_test: tests/compass_test.c src/sensors/compass.c src/sensors/compass.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/sensors/compass.c -lm -o $@

$(BUILD)/test/fusion_test: tests/fusion_test.c src/sensors/fusion.c src/sensors/fusion.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/sensors/fusion.c -lm -o $@

$(BUILD)/test/sensor_power_test: tests/sensor_power_test.c src/sensors/ak09918.c src/sensors/hub_lsm6dso.c $(wildcard src/*.h src/board/*.h src/sensors/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(subst src/soc/qcom,src/soc/apollo3,$(HOST_CFLAGS)) $< src/sensors/ak09918.c src/sensors/hub_lsm6dso.c -o $@

$(BUILD)/test/flatmesh_mcu_test: tests/flatmesh_mcu_test.c src/display/flatmesh_mcu.c $(wildcard src/display/flatmesh_mcu*.h src/ui/ui.h src/ui/codes.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) -Wno-unknown-pragmas $< src/display/flatmesh_mcu.c -o $@

COMPASS_APP_TEST := src/compass_app.c src/sensors/compass.c src/sensors/fusion.c
$(BUILD)/test/compass_app_test: tests/compass_app_test.c $(COMPASS_APP_TEST) $(wildcard src/*.h src/ui/*.h src/sensors/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< $(COMPASS_APP_TEST) -lm -o $@

COMPANION_TEST := $(COMPANION) src/input/haptics.c src/input/haptics_aw8624.c
$(BUILD)/test/companion_test: tests/companion_test.c $(COMPANION_TEST) $(wildcard src/companion/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(subst -I src/soc/qcom,-I src/soc/apollo3,$(HOST_CFLAGS)) $< $(COMPANION_TEST) -o $@

$(BUILD)/test/apollo3_timer_test: tests/apollo3_timer_test.c src/soc/apollo3/timer.c $(wildcard tests/*.h src/*.h src/soc/apollo3/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(subst -I src/soc/qcom,-I src/soc/apollo3,$(subst tests/host.h,tests/host_mmio.h,$(HOST_CFLAGS))) $< src/soc/apollo3/timer.c -o $@

$(BUILD)/test/swdwake_test: tests/swdwake_test.c src/soc/apollo3/swdwake.c src/sched.c $(wildcard tests/*.h src/*.h src/soc/apollo3/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(subst tests/host.h,tests/host_mmio.h,$(subst -I src/soc/qcom,-I src/soc/apollo3,$(HOST_CFLAGS))) $< src/soc/apollo3/swdwake.c src/sched.c -o $@

$(BUILD)/test/rm69090_test: tests/rm69090_test.c src/display/rm69090_id.c src/display/rm69090_id.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/display/rm69090_id.c -o $@

$(BUILD)/test/tma525_test: tests/tma525_test.c src/input/tma525_proto.c src/input/tma525_proto.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/input/tma525_proto.c -o $@

BELUGA_DETECT_TEST := src/soc/apollo3/detect.c src/display/rm69090_id.c src/input/tma525_proto.c
$(BUILD)/test/beluga_detect_test: tests/beluga_detect_test.c $(BELUGA_DETECT_TEST) $(wildcard tests/*.h src/*.h src/*/*.h src/soc/apollo3/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(subst -I src/soc/qcom,-I src/soc/apollo3,$(HOST_CFLAGS)) $< $(BELUGA_DETECT_TEST) -o $@

# detect.c is included by the test itself.
$(BUILD)/test/beluga_display_test: tests/beluga_display_test.c src/display/display_rm69090.c $(BELUGA_DETECT_TEST) $(wildcard tests/*.h src/*.h src/*/*.h src/soc/apollo3/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(subst -I src/soc/qcom,-I src/soc/apollo3,$(HOST_CFLAGS)) $< $(filter-out %/detect.c,$(BELUGA_DETECT_TEST)) -o $@
test: $(BUILD)/test/beluga_display_test

$(BUILD)/test/swd_test: tests/swd_test.c tools/beluga/swd.c tools/beluga/swd.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin -include tests/host.h,$(HOST_CFLAGS)) $< tools/beluga/swd.c -o $@

$(BUILD)/test/small.ext4: tests/data/small.ext4.gz
	@mkdir -p $(dir $@)
	gunzip -c $< > $@

$(BUILD)/test/small.ext2: tests/data/small.ext2.gz
	@mkdir -p $(dir $@)
	gunzip -c $< > $@

$(BUILD)/test/%: tests/%.c $(HOST_SRCS) $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< $(HOST_SRCS) -o $@

# Run alongside each other; each test's output is shown whole once it is done.
TESTS := console clockapps calc game agenda orient rotation actions power \
	   sched events steps fitness workout hrlog hr_accept gauge stc3117 bq27421 backlight \
	   mxt fts hx852x smem mdt smd wcnss_msg m4 nanohub tsl2584 al3320 tsl258x \
	   auto_brightness ramoops soft sgx_compose qcom_compose emmc squashfs \
	   sgx_image sgx_draw sgx sgx_stack sgx_paint swd bleif companion \
	   apollo3_timer swdwake hr raise compass fusion compass_app sensor_power \
	   flatmesh_mcu rm69090 beluga_display beluga_detect tma525 stack adb
test: $(addprefix test-,$(TESTS)) test-board test-boot test-parser
$(addprefix test-,$(TESTS)): test-%: $(BUILD)/test/%_test
	@$< > $<.log 2>&1; s=$$?; cat $<.log; exit $$s
test-board: $(BUILD)/test/board_test $(BUILD)/smelt.dtb $(BUILD)/sturgeon.dtb $(BUILD)/dory.dtb $(BUILD)/catfish.dtb $(BUILD)/swift.dtb $(BUILD)/lenok.dtb
	@$^ > $<.log 2>&1; s=$$?; cat $<.log; exit $$s
test-boot: $(BUILD)/qcdt.img
	@$(PYTHON) tests/boot_test.py $< > $(BUILD)/boot_test.log 2>&1; s=$$?; cat $(BUILD)/boot_test.log; exit $$s
test-parser: $(BUILD)/test/parser_test $(BUILD)/test/small.ext4 $(BUILD)/test/small.ext2
	@$^ > $<.log 2>&1; s=$$?; cat $<.log; exit $$s
.PHONY: $(addprefix test-,$(TESTS)) test-board test-boot test-parser

# The tool for the OPPO Watch's coprocessor runs on the watch's Linux, so
# it takes a compiler for that: the one in a beluga build's sysroot.
SWD_CC ?= arm-linux-gnueabihf-gcc
build/swdtool: tools/beluga/swdtool.c tools/beluga/swd.c tools/beluga/swd.h
	@mkdir -p $(dir $@)
	$(SWD_CC) -static -O2 -Wall -Wextra -Wno-unused-parameter tools/beluga/swdtool.c tools/beluga/swd.c -o $@
swdtool: build/swdtool

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
.PHONY: all run preview textbench disasm test clean swdtool ramtest FORCE bench-soft
