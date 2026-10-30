# SPDX-License-Identifier: MIT
NAME := OpenPS2BIOS
VERSION := 0.5

CC := clang
LD := ld.lld
OBJCOPY := llvm-objcopy
PYTHON := python3
BUILD := build

CFLAGS := --target=mipsel-none-elf -mabi=32 -mno-abicalls -fno-pic -G0 \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-common \
          -msoft-float -Os -ffunction-sections -fdata-sections -Wall -Wextra -Werror \
          -Iinclude -DOB_VERSION='"$(VERSION)"'
# Each function and object gets its own section, so the linker drops what
# nothing references.
LDFLAGS := -m elf32ltsmip --gc-sections

COMMON := src/common/compiler.c src/common/base.c src/common/rom.c src/common/console.c
EE_SRC := $(COMMON) src/common/elf.c src/common/sched.c src/common/alarm.c src/common/deci2.c \
          src/common/iso9660.c src/common/sifdma.c src/ee/sif.c src/ee/deci2.c src/ee/alarm.c \
          src/ee/discio.c src/ee/disc.c src/ee/main.c src/ee/kernel.c src/ee/start.S src/ee/exception.S
IOP_SRC := $(COMMON) src/common/iso9660.c src/common/sifcmd.c src/common/sifrpc.c src/common/irx.c \
           src/common/format.c src/common/iopk.c src/iop/sysmem.c src/iop/loadcore.c src/iop/libs.c \
           src/iop/kernel.c src/iop/heaplib.c src/iop/dmacman.c src/iop/cdvdman.c src/iop/ioman.c \
           src/iop/fileio.c src/iop/modload.c src/iop/secrman.c src/iop/timrman.c src/iop/sifrpc.c \
           src/iop/exception.S src/iop/rpc.c src/iop/sif.c src/iop/disc.c src/iop/main.c src/iop/start.S
OSD_SRC := osd/start.S osd/osd.c src/common/base.c src/common/compiler.c src/common/console.c \
           src/common/iso9660.c src/ee/discio.c

EE_OBJ := $(patsubst src/%,$(BUILD)/ee/%.o,$(EE_SRC))
IOP_OBJ := $(patsubst src/%,$(BUILD)/iop/%.o,$(IOP_SRC))
OSD_OBJ := $(patsubst %,$(BUILD)/osd/%.o,$(OSD_SRC))

.PHONY: all clean
all: $(BUILD)/$(NAME).bin

$(BUILD)/ee/%.o: src/%
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -march=mips2 -MMD -MP -MF $@.d -c $< -o $@

$(BUILD)/iop/%.o: src/%
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -march=mips1 -DOB_IOP -MMD -MP -MF $@.d -c $< -o $@

$(BUILD)/osd/%.o: %
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -march=mips2 -MMD -MP -MF $@.d -c $< -o $@

$(BUILD)/reset.o: src/reset.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -march=mips1 -c $< -o $@

$(BUILD)/reset.elf: $(BUILD)/reset.o src/reset.ld
	$(LD) $(LDFLAGS) -T src/reset.ld $< -o $@

$(BUILD)/ee.elf: $(EE_OBJ) src/runtime.ld
	$(LD) $(LDFLAGS) -T src/runtime.ld $(EE_OBJ) -o $@

$(BUILD)/iop.elf: $(IOP_OBJ) src/iop/runtime.ld
	$(LD) $(LDFLAGS) -T src/iop/runtime.ld $(IOP_OBJ) -o $@

$(BUILD)/osdsys.elf: $(OSD_OBJ) osd/osd.ld
	$(LD) $(LDFLAGS) -T osd/osd.ld $(OSD_OBJ) -o $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(NAME).bin: $(BUILD)/reset.bin $(BUILD)/ee.bin $(BUILD)/iop.bin $(BUILD)/osdsys.elf tools/rompack.py
	$(PYTHON) tools/rompack.py --reset $(BUILD)/reset.bin --output $@ \
	  --module EELOAD=$(BUILD)/ee.bin --module IOPBOOT=$(BUILD)/iop.bin \
	  --module OSDSYS=$(BUILD)/osdsys.elf

clean:
	rm -rf $(BUILD)

-include $(EE_OBJ:%=%.d) $(IOP_OBJ:%=%.d) $(OSD_OBJ:%=%.d)
