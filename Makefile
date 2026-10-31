# asteroid-embedded: bare-metal firmware for APQ8026 smartwatches.
# make            build/asteroid.img for WATCH (sturgeon by default)
# make run        boot it over fastboot, wait for the reboot, show the log
# make test       host unit tests of the console, adb and the parsers
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
DTC     ?= dtc
WATCH   ?= sturgeon
TEST    ?=
include src/board/$(WATCH)/board.mk

BUILD  := build
TUSB   := third_party/tinyusb/src
NIMBLE := third_party/nimble
MBEDTLS := third_party/mbedtls
GITREV := $(shell git rev-parse --short=10 HEAD 2>/dev/null || echo nogit)
DIRTY  := $(shell git diff --quiet HEAD 2>/dev/null || echo +)

SRCS := src/start.S src/soc/cache.S src/main.c src/version.c src/settings.c src/alarm.c src/clockapps.c src/calc.c src/power.c src/sensors/steps.c src/civil.c \
        src/soc/mmu.c src/soc/gic.c src/soc/timer.c src/soc/wdt.c \
        src/soc/spmi.c src/soc/pmic.c src/soc/bms.c src/soc/reboot.c src/soc/scm.c src/soc/exceptions.c \
        src/soc/tlmm.c src/soc/clock.c src/soc/spm.c src/soc/suspend.S src/soc/i2c_qup.c src/soc/uart_dm.c src/soc/rpm.c src/board/stubs.c $(BOARD_SRCS) \
        src/log.c src/display/display.c src/display/dsi.c src/display/fbcon.c src/display/flatmesh.c \
        src/lib/libc.c src/lib/fmt.c src/ui/ui.c src/ui/stack.c \
        src/gpu/a3xx.c src/gpu/draw.c src/gpu/paint.c \
        src/bt/hci.c src/bt/bcm.c src/bt/npl.c src/bt/transport.c src/bt/store.c src/bt/gatt.c src/bt/host.c \
        src/usb/usb.c src/usb/dcd_ci_hs_msm.c src/usb/adb.c src/usb/ramfs.c src/console/console.c \
        src/notify/notify.c src/media/media.c src/weather/weather.c src/screenshot/screenshot.c src/screenshot/jpeg.c \
        src/storage/emmc.c src/storage/ext4.c src/storage/rootfs.c src/storage/firmware.c src/storage/store.c src/storage/kv.c \
        $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c \
        $(TUSB)/class/cdc/cdc_device.c
NIMBLE_SRCS := $(filter-out %/ble_gatts_lcl.c,$(wildcard $(NIMBLE)/nimble/host/src/*.c)) \
        $(NIMBLE)/nimble/host/services/gap/src/ble_svc_gap.c $(NIMBLE)/nimble/host/services/gatt/src/ble_svc_gatt.c \
        $(NIMBLE)/nimble/transport/src/transport.c \
        $(addprefix $(NIMBLE)/porting/nimble/src/,endian.c mem.c nimble_port.c os_mbuf.c os_mempool.c os_msys.c)
MBEDTLS_SRCS := $(addprefix $(MBEDTLS)/library/,aes.c bignum.c bignum_core.c cipher.c cipher_wrap.c cmac.c \
        constant_time.c ecdh.c ecp.c ecp_curves.c memory_buffer_alloc.c platform.c platform_util.c)
SRCS += $(NIMBLE_SRCS) $(MBEDTLS_SRCS)
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS))

CFLAGS := -mcpu=cortex-a7 -marm -mfloat-abi=hard -mfpu=neon-vfpv4 -ffreestanding -nostdlib \
          -Os -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -ffunction-sections -fdata-sections -fno-common \
          -Isrc -Isrc/board/$(WATCH) -I$(BUILD) -Isrc/usb -I$(TUSB) -I$(TUSB)/portable/chipidea/ci_hs -Isrc/bt/port \
          $(addprefix -isystem $(NIMBLE)/,nimble/include nimble/host/include nimble/host/services/gap/include \
            nimble/host/services/gatt/include nimble/transport/include porting/nimble/include) \
          -isystem $(MBEDTLS)/include -DMBEDTLS_CONFIG_FILE='"bt_mbedtls_config.h"'
LDFLAGS := -nostdlib -Wl,--gc-sections -Wl,-T,$(BUILD)/asteroid.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-z,noexecstack
LIBS    := -lgcc

CARGO   ?= $(or $(shell command -v cargo 2>/dev/null),/opt/homebrew/opt/rustup/bin/cargo)
UI_LIB  := ui/target/armv7a-none-eabihf/release/libasteroid_ui.a
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
$(BUILD)/watch.mk: FORCE
	@mkdir -p $(BUILD)
	@echo 'WATCH := $(WATCH)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
FORCE:

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@


THIRD_PARTY_CFLAGS ?= -w
$(BUILD)/$(NIMBLE)/%.c.o $(BUILD)/$(MBEDTLS)/%.c.o: CFLAGS += $(THIRD_PARTY_CFLAGS)

$(BUILD)/src/version.c.o: $(BUILD)/gitrev.h
$(BUILD)/src/%.c.o: CFLAGS += -Werror
$(BUILD)/%.c.o: %.c $(BUILD)/config.h $(BUILD)/watch.mk
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

$(UI_LIB): $(UI_SRCS) $(BUILD)/watch.mk
	cd ui && PATH="$(dir $(CARGO)):$$PATH" ASTEROID_BOARD=$(WATCH) ASTEROID_UI_SIZE=$(BOARD_UI_SIZE) \
		$(CARGO) build --release --target armv7a-none-eabihf
	@touch $@

$(BUILD)/asteroid.ld: ld/asteroid.ld.in src/soc/memmap.h
	@mkdir -p $(BUILD)
	$(CC) -E -P -x c -DLINKER_SCRIPT -Isrc $< -o $@

$(BUILD)/asteroid.elf: $(OBJS) $(UI_LIB) $(BUILD)/asteroid.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) $(UI_LIB) $(LIBS) -o $@
	$(SIZE) $@

$(BUILD)/asteroid.bin: $(BUILD)/asteroid.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/%.dtb: dts/%.dts
	@mkdir -p $(BUILD)
	$(DTC) -q -I dts -O dtb -o $@ $<

$(BUILD)/qcdt-$(WATCH).img: $(BUILD)/$(WATCH).dtb tools/qcdt.py
	python3 tools/qcdt.py -o $@ $<

$(BUILD)/asteroid.img: $(BUILD)/asteroid.bin $(BUILD)/qcdt-$(WATCH).img tools/mkbootimg.py
	python3 tools/mkbootimg.py --kernel $(BUILD)/asteroid.bin --dt $(BUILD)/qcdt-$(WATCH).img -o $@

run: $(BUILD)/asteroid.img
	python3 tools/run.py $(RUNFLAGS)

SLINT_VIEWER ?= $(or $(shell command -v slint-viewer 2>/dev/null),$(HOME)/.cargo/bin/slint-viewer)

preview: $(UI_LIB)
	$(SLINT_VIEWER) -I $(abspath $(dir $(firstword $(wildcard ui/target/armv7a-none-eabihf/release/build/asteroid-ui-*/out/note_icons.slint)))) \
		--auto-reload --load-data tools/preview/watchface.json ui/ui/preview.slint

disasm: $(BUILD)/asteroid.elf
	$(OBJDUMP) -d $< > $(BUILD)/asteroid.dis

HOSTCC      ?= cc
HOST_CFLAGS := -std=gnu11 -g -Wall -Wextra -Wno-unused-parameter -Wno-format -fno-builtin \
               -U_FORTIFY_SOURCE -I src -I src/board/$(WATCH) -I tests -DHOST_TEST -include tests/host.h \
               -fsanitize=address,undefined -fno-sanitize-recover=all
HOST_SRCS   := tests/stubs.c src/console/console.c src/usb/adb.c src/usb/ramfs.c src/lib/fmt.c src/clockapps.c src/alarm.c src/ui/stack.c src/calc.c
PARSER_SRCS := src/notify/notify.c src/weather/weather.c src/media/media.c src/storage/kv.c src/storage/ext4.c

$(BUILD)/test/parser_test: tests/parser_test.c $(PARSER_SRCS) $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(filter-out -fno-builtin,$(HOST_CFLAGS)) $< $(PARSER_SRCS) -o $@

$(BUILD)/test/small.ext4: tests/data/small.ext4.gz
	@mkdir -p $(dir $@)
	gunzip -c $< > $@

$(BUILD)/test/%: tests/%.c $(HOST_SRCS) $(wildcard tests/*.h src/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< $(HOST_SRCS) -o $@

test: $(BUILD)/test/console_test $(BUILD)/test/adb_test $(BUILD)/test/clockapps_test $(BUILD)/test/stack_test $(BUILD)/test/parser_test $(BUILD)/test/calc_test $(BUILD)/test/small.ext4
	$(BUILD)/test/console_test
	$(BUILD)/test/clockapps_test
	$(BUILD)/test/calc_test
	$(BUILD)/test/stack_test
	$(BUILD)/test/adb_test
	$(BUILD)/test/parser_test $(BUILD)/test/small.ext4

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
.PHONY: all run preview disasm test clean FORCE
