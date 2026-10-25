# asteroid-embedded: bare-metal firmware for APQ8026 smartwatches.
# make            build/asteroid.img for WATCH (sturgeon by default)
# make run        boot it over fastboot, wait for the reboot, show the log
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
DTC     ?= dtc
WATCH   ?= sturgeon
TEST    ?=

BUILD  := build
GITREV := $(shell git rev-parse --short=10 HEAD 2>/dev/null || echo nogit)
DIRTY  := $(shell git diff --quiet HEAD 2>/dev/null || echo +)

SRCS := src/start.S src/soc/cache.S src/main.c \
        src/soc/mmu.c src/soc/gic.c src/soc/timer.c src/soc/wdt.c \
        src/soc/spmi.c src/soc/pmic.c src/soc/reboot.c src/soc/exceptions.c \
        src/log.c src/display/mdp.c src/display/fbcon.c \
        src/lib/libc.c src/lib/fmt.c
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS))

CFLAGS := -mcpu=cortex-a7 -marm -mfloat-abi=soft -ffreestanding -nostdlib \
          -Os -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -ffunction-sections -fdata-sections -fno-common \
          -Isrc -I$(BUILD)
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

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
.PHONY: all run disasm clean FORCE
