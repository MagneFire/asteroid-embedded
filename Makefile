# asteroid-embedded: bare-metal firmware for APQ8026 smartwatches.
# make            build/asteroid.img for WATCH (sturgeon by default)
# make run        boot it over fastboot, wait for the reboot, show the log
# make test       host unit tests of the console and adb
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
DTC     ?= dtc
WATCH   ?= sturgeon
TEST    ?=

BUILD  := build
TUSB   := third_party/tinyusb/src
GITREV := $(shell git rev-parse --short=10 HEAD 2>/dev/null || echo nogit)
DIRTY  := $(shell git diff --quiet HEAD 2>/dev/null || echo +)

SRCS := src/start.S src/soc/cache.S src/main.c \
        src/soc/mmu.c src/soc/gic.c src/soc/timer.c src/soc/wdt.c \
        src/soc/spmi.c src/soc/pmic.c src/soc/reboot.c src/soc/scm.c src/soc/exceptions.c \
        src/soc/tlmm.c src/soc/i2c_gpio.c src/soc/i2c_qup.c src/soc/rpm.c src/input/touch.c \
        src/log.c src/display/display.c src/display/dsi.c src/display/fbcon.c src/display/flatmesh.c \
        src/lib/libc.c src/lib/fmt.c \
        src/usb/usb.c src/usb/dcd_ci_hs_msm.c src/usb/adb.c src/usb/ramfs.c src/console/console.c \
        $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c \
        $(TUSB)/class/cdc/cdc_device.c
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS))

CFLAGS := -mcpu=cortex-a7 -marm -mfloat-abi=hard -mfpu=neon-vfpv4 -ffreestanding -nostdlib \
          -Os -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -ffunction-sections -fdata-sections -fno-common \
          -Isrc -I$(BUILD) -Isrc/usb -I$(TUSB) -I$(TUSB)/portable/chipidea/ci_hs
LDFLAGS := -nostdlib -Wl,--gc-sections -Wl,-T,ld/asteroid.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments
LIBS    := -lgcc

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
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.c.o: %.c $(BUILD)/gitrev.h $(BUILD)/config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

$(BUILD)/asteroid.elf: $(OBJS) ld/asteroid.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) -o $@
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

disasm: $(BUILD)/asteroid.elf
	$(OBJDUMP) -d $< > $(BUILD)/asteroid.dis

HOSTCC      ?= cc
HOST_CFLAGS := -std=gnu11 -g -Wall -Wextra -Wno-unused-parameter -Wno-format -fno-builtin \
               -U_FORTIFY_SOURCE -I src -I tests -DHOST_TEST -include tests/host.h
HOST_SRCS   := tests/stubs.c src/console/console.c src/usb/adb.c src/usb/ramfs.c src/lib/fmt.c

$(BUILD)/test/%: tests/%.c $(HOST_SRCS) tests/host.h tests/stubs.h src/asteroid.h src/usb/adb.h src/console/console.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $< $(HOST_SRCS) -o $@

test: $(BUILD)/test/console_test $(BUILD)/test/adb_test
	$(BUILD)/test/console_test
	$(BUILD)/test/adb_test

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
.PHONY: all run disasm test clean FORCE
