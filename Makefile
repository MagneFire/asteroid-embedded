# asteroid-embedded: bare-metal firmware for APQ8026 smartwatches.
# make            build/asteroid.img, for every supported watch
# make run        boot it over fastboot, wait for the reboot, show the log
# make test       host unit tests of the console, adb and the parsers
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
DTC     ?= dtc
TEST    ?=

# The SoC the image is for: qcom (APQ8026) or omap3 (OMAP3630). Its code
# is in src/soc/$(SOC), and each has its own image.
SOC    ?= qcom
BUILD  := build$(if $(filter qcom,$(SOC)),,/$(SOC))
TUSB   := third_party/tinyusb/src
NIMBLE := third_party/nimble
MBEDTLS := third_party/mbedtls
GITREV := $(shell git rev-parse --short=10 HEAD 2>/dev/null || echo nogit)
DIRTY  := $(shell git diff --quiet HEAD 2>/dev/null || echo +)

SRCS := src/start.S src/soc/cache.S src/main.c src/version.c src/settings.c src/alarm.c src/clockapps.c src/calc.c src/game.c \
        src/power.c src/power_policy.c src/sched.c src/frame.c src/stats.c src/app.c src/actions.c src/events.c src/present.c \
        src/civil.c src/log.c src/soc/mmu.c src/soc/exceptions.c src/board/drivers.c \
        src/sensors/steps.c src/sensors/hub_m4.c src/sensors/m4_proto.c src/sensors/auto_brightness.c src/sensors/orient.c \
        src/input/mxt.c src/input/touch_mxt.c src/input/haptics.c \
        src/soc/max17050.c src/soc/charge_policy.c src/soc/gauge_max17050.c \
        src/display/backlight_lm3535.c src/sensors/tsl2584.c src/audio/speaker.c src/display/fbcon.c src/display/flatmesh.c \
        src/lib/libc.c src/lib/fmt.c src/lib/fdt.c src/ui/ui.c src/ui/stack.c src/ui/rotation.c \
        src/bt/hci.c src/bt/npl.c src/bt/transport.c src/bt/store.c src/bt/gatt.c src/bt/host.c \
        src/usb/usb.c src/usb/adb.c src/usb/ramfs.c src/console/console.c \
        src/notify/notify.c src/media/media.c src/weather/weather.c src/screenshot/screenshot.c src/screenshot/jpeg.c \
        src/storage/emmc.c src/storage/ext4.c src/storage/rootfs.c src/storage/firmware.c src/storage/store.c src/storage/kv.c \
        $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c \
        $(TUSB)/class/cdc/cdc_device.c

SRCS_qcom := $(addprefix src/soc/qcom/,init.c mmu.c gic.c timer.c wdt.c spmi.c pmic.c bms.c reboot.c scm.c tlmm.c clock.c spm.c \
               suspend.S i2c_qup.c uart_dm.c rpm.c) \
        src/board/detect.c src/board/sturgeon.c src/board/sparrow.c src/board/smelt.c \
        src/sensors/hub.c src/sensors/hub_em7184.c src/sensors/light_tsl2584.c \
        src/input/touch.c src/input/touch_it7260.c src/input/haptics_drv2605.c src/input/haptics_pmic.c \
        src/display/display.c src/display/dsi.c src/gpu/a3xx.c src/gpu/draw.c src/gpu/paint.c \
        src/bt/bcm.c src/usb/dcd_ci_hs_msm.c src/usb/port_ci_hs_msm.c src/storage/emmc_msm.c
CPU_qcom  := cortex-a7
FPU_qcom  := neon-vfpv4
TUSB_PORT_qcom := chipidea/ci_hs
TUSB_DEFS_qcom := -DTUP_USBIP_CHIPIDEA_HS -DTUP_DCD_ENDPOINT_MAX=16

SRCS_omap3 :=
CPU_omap3 := cortex-a8
FPU_omap3 := neon
TUSB_PORT_omap3 := mentor/musb
# The Mentor driver keeps its endpoint interrupts in 8 bits.
TUSB_DEFS_omap3 := -DTUP_USBIP_MUSB -DTUP_USBIP_MUSB_TI -DTUP_DCD_ENDPOINT_MAX=8

SRCS += $(SRCS_$(SOC))
NIMBLE_SRCS := $(filter-out %/ble_gatts_lcl.c,$(wildcard $(NIMBLE)/nimble/host/src/*.c)) \
        $(NIMBLE)/nimble/host/services/gap/src/ble_svc_gap.c $(NIMBLE)/nimble/host/services/gatt/src/ble_svc_gatt.c \
        $(NIMBLE)/nimble/transport/src/transport.c \
        $(addprefix $(NIMBLE)/porting/nimble/src/,endian.c mem.c nimble_port.c os_mbuf.c os_mempool.c os_msys.c)
MBEDTLS_SRCS := $(addprefix $(MBEDTLS)/library/,aes.c bignum.c bignum_core.c cipher.c cipher_wrap.c cmac.c \
        constant_time.c ecdh.c ecp.c ecp_curves.c memory_buffer_alloc.c platform.c platform_util.c)
SRCS += $(NIMBLE_SRCS) $(MBEDTLS_SRCS)
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS))

CFLAGS := -mcpu=$(CPU_$(SOC)) -marm -mfloat-abi=hard -mfpu=$(FPU_$(SOC)) -ffreestanding -nostdlib \
          -Os -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -ffunction-sections -fdata-sections -fno-common \
          -Isrc -Isrc/soc/$(SOC) -I$(BUILD) -Isrc/usb -I$(TUSB) -I$(TUSB)/portable/$(TUSB_PORT_$(SOC)) -Isrc/bt/port \
          $(addprefix -isystem $(NIMBLE)/,nimble/include nimble/host/include nimble/host/services/gap/include \
            nimble/host/services/gatt/include nimble/transport/include porting/nimble/include) \
          -isystem $(MBEDTLS)/include -DMBEDTLS_CONFIG_FILE='"bt_mbedtls_config.h"' $(TUSB_DEFS_$(SOC))
LDFLAGS := -nostdlib -Wl,--gc-sections -Wl,-T,$(BUILD)/asteroid.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-z,noexecstack
LIBS    := -lgcc

# The UI is built once per CPU: the A8 has neither the A7's divide nor
# its fused multiply-add.
CARGO   ?= $(or $(shell command -v cargo 2>/dev/null),/opt/homebrew/opt/rustup/bin/cargo)
UI_TARGET := ui/target$(if $(filter qcom,$(SOC)),,-$(SOC))
UI_LIB  := $(UI_TARGET)/armv7a-none-eabihf/release/libasteroid_ui.a
UI_SRCS := ui/Cargo.toml ui/Cargo.lock ui/build.rs ui/.cargo/config.toml \
           $(shell find ui/src ui/ui ui/fonts ui/icons third_party/slint -type f 2>/dev/null)

all: $(BUILD)/asteroid.img

$(BUILD)/gitrev.h: FORCE
	@mkdir -p $(BUILD)
	@echo '#define GITREV "$(GITREV)$(DIRTY)"' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
$(BUILD)/config.h: FORCE
	@mkdir -p $(BUILD)
	@echo '$(if $(TEST),#define TEST_$(shell echo $(TEST) | tr a-z A-Z) 1,)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
FORCE:

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c $< -o $@


THIRD_PARTY_CFLAGS ?= -w
$(BUILD)/$(NIMBLE)/%.c.o $(BUILD)/$(MBEDTLS)/%.c.o: CFLAGS += $(THIRD_PARTY_CFLAGS)

$(BUILD)/src/version.c.o: $(BUILD)/gitrev.h
$(BUILD)/src/%.c.o: CFLAGS += -Werror
$(BUILD)/%.c.o: %.c $(BUILD)/config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

$(UI_LIB): $(UI_SRCS)
	cd ui && PATH="$(dir $(CARGO)):$$PATH" CARGO_TARGET_ARMV7A_NONE_EABIHF_RUSTFLAGS="-C target-cpu=$(CPU_$(SOC))" \
		$(CARGO) build --release --target armv7a-none-eabihf --target-dir $(abspath $(UI_TARGET))
	@touch $@

$(BUILD)/asteroid.ld: ld/asteroid.ld.in src/soc/$(SOC)/memmap.h
	@mkdir -p $(BUILD)
	$(CC) -E -P -x c -DLINKER_SCRIPT -Isrc/soc/$(SOC) $< -o $@

$(BUILD)/asteroid.elf: $(OBJS) $(UI_LIB) $(BUILD)/asteroid.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) $(UI_LIB) $(LIBS) -o $@
	$(SIZE) $@

$(BUILD)/asteroid.bin: $(BUILD)/asteroid.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/%.dtb: dts/%.dts
	@mkdir -p $(BUILD)
	$(DTC) -q -I dts -O dtb -o $@ $<

# One tree per watch: each bootloader picks the one whose board-id it has.
# A new tree goes last, so the entries before it stay as they are.
DTBS := $(BUILD)/sparrow.dtb $(BUILD)/sturgeon.dtb $(BUILD)/smelt.dtb

$(BUILD)/qcdt.img: $(DTBS) tools/qcdt.py
	python3 tools/qcdt.py -o $@ $(DTBS)

# One image for every watch. Bootloaders that read the table use it, and
# ignore what follows the firmware. smelt's own images have no table and
# the tree behind the kernel, so that is there too, in case its bootloader
# only looks there.
$(BUILD)/asteroid-tree.bin: $(BUILD)/asteroid.bin $(BUILD)/smelt.dtb tools/appenddtb.py
	python3 tools/appenddtb.py -o $@ $< $(BUILD)/smelt.dtb

$(BUILD)/asteroid.img: $(BUILD)/asteroid-tree.bin $(BUILD)/qcdt.img tools/mkbootimg.py
	python3 tools/mkbootimg.py --kernel $< --dt $(BUILD)/qcdt.img -o $@

run: $(BUILD)/asteroid.img
	python3 tools/run.py $(RUNFLAGS)

SLINT_VIEWER ?= $(or $(shell command -v slint-viewer 2>/dev/null),$(HOME)/.cargo/bin/slint-viewer)

# The viewer lays the UI out on Theme's default screen; icons are drawn by the firmware, so the
# viewer shows none.
preview: $(UI_LIB)
	$(SLINT_VIEWER) -I $(abspath $(dir $(firstword $(wildcard ui/target/armv7a-none-eabihf/release/build/asteroid-ui-*/out/codes.slint)))) \
		--auto-reload --load-data tools/preview/watchface.json ui/ui/preview.slint

disasm: $(BUILD)/asteroid.elf
	$(OBJDUMP) -d $< > $(BUILD)/asteroid.dis

HOSTCC      ?= cc
HOST_CFLAGS := -std=gnu11 -g -Wall -Wextra -Wno-unused-parameter -Wno-format -fno-builtin \
               -U_FORTIFY_SOURCE -I src -I src/soc/qcom -I tests -DHOST_TEST -include tests/host.h \
               -fsanitize=address,undefined -fno-sanitize-recover=all
HOST_SRCS   := tests/stubs.c src/console/console.c src/usb/adb.c src/usb/ramfs.c src/lib/fmt.c src/clockapps.c src/alarm.c src/ui/stack.c src/calc.c src/game.c src/sensors/orient.c src/ui/rotation.c
PARSER_SRCS := src/notify/notify.c src/weather/weather.c src/media/media.c src/storage/kv.c src/storage/ext4.c

$(BUILD)/test/parser_test: tests/parser_test.c $(PARSER_SRCS) $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< $(PARSER_SRCS) -o $@

$(BUILD)/test/actions_test: tests/actions_test.c src/actions.c src/civil.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/actions.c src/civil.c -o $@

$(BUILD)/test/power_test: tests/power_test.c src/power_policy.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/power_policy.c -o $@

$(BUILD)/test/sched_test: tests/sched_test.c src/sched.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/sched.c -o $@

$(BUILD)/test/events_test: tests/events_test.c src/events.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/events.c -o $@

$(BUILD)/test/gauge_test: tests/gauge_test.c src/soc/max17050.c src/soc/charge_policy.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/soc/max17050.c src/soc/charge_policy.c -o $@

$(BUILD)/test/backlight_test: tests/backlight_test.c src/display/backlight_lm3535.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< -o $@

$(BUILD)/test/mxt_test: tests/mxt_test.c src/input/mxt.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/input/mxt.c -o $@

$(BUILD)/test/m4_test: tests/m4_test.c src/sensors/m4_proto.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/sensors/m4_proto.c -o $@

$(BUILD)/test/tsl2584_test: tests/tsl2584_test.c src/sensors/light_tsl2584.c src/sensors/tsl2584.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/sensors/tsl2584.c -o $@

$(BUILD)/test/auto_brightness_test: tests/auto_brightness_test.c src/sensors/auto_brightness.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< src/sensors/auto_brightness.c -o $@

$(BUILD)/test/board_test: tests/board_test.c src/board/detect.c src/board/sturgeon.c src/board/sparrow.c src/board/smelt.c src/lib/fdt.c $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< src/board/detect.c src/board/sturgeon.c src/board/sparrow.c src/board/smelt.c src/lib/fdt.c -o $@

$(BUILD)/test/small.ext4: tests/data/small.ext4.gz
	@mkdir -p $(dir $@)
	gunzip -c $< > $@

$(BUILD)/test/%: tests/%.c $(HOST_SRCS) $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< $(HOST_SRCS) -o $@

test: $(BUILD)/test/console_test $(BUILD)/test/adb_test $(BUILD)/test/clockapps_test $(BUILD)/test/stack_test $(BUILD)/test/parser_test $(BUILD)/test/calc_test $(BUILD)/test/game_test $(BUILD)/test/orient_test $(BUILD)/test/rotation_test $(BUILD)/test/actions_test $(BUILD)/test/power_test $(BUILD)/test/sched_test $(BUILD)/test/events_test $(BUILD)/test/small.ext4 $(BUILD)/test/board_test $(BUILD)/qcdt.img $(BUILD)/test/gauge_test $(BUILD)/test/backlight_test $(BUILD)/test/mxt_test $(BUILD)/test/m4_test $(BUILD)/test/tsl2584_test $(BUILD)/test/auto_brightness_test
	$(BUILD)/test/console_test
	$(BUILD)/test/clockapps_test
	$(BUILD)/test/calc_test
	$(BUILD)/test/game_test
	$(BUILD)/test/orient_test
	$(BUILD)/test/rotation_test
	$(BUILD)/test/actions_test
	$(BUILD)/test/power_test
	$(BUILD)/test/sched_test
	$(BUILD)/test/events_test
	$(BUILD)/test/gauge_test
	$(BUILD)/test/backlight_test
	$(BUILD)/test/mxt_test
	$(BUILD)/test/m4_test
	$(BUILD)/test/tsl2584_test
	$(BUILD)/test/auto_brightness_test
	$(BUILD)/test/board_test $(BUILD)/smelt.dtb $(BUILD)/sturgeon.dtb
	python3 tests/boot_test.py $(BUILD)/qcdt.img
	$(BUILD)/test/stack_test
	$(BUILD)/test/adb_test
	$(BUILD)/test/parser_test $(BUILD)/test/small.ext4

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
.PHONY: all run preview disasm test clean FORCE
