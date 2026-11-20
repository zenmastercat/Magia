# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 zenmastercat and Magia contributors
#
# Magia build.
#   make          build build/magia.elf (ELF64) and build/magia32.elf (for QEMU -kernel)
#   make run      boot in QEMU, serial console on stdio (Ctrl-A x quits)
#   make test     host cargo tests + headless QEMU ktest run
#   make check    alias for test

CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy
NM      ?= nm
CARGO   ?= cargo
QEMU    ?= qemu-system-x86_64
QEMU_ACCEL ?= tcg
QEMU_MEM   ?= 256M

# GNU make defaults CC to "cc"; insist on gcc for the kernel flags below.
ifeq ($(origin CC),default)
CC := gcc
endif

BUILD := build
RUST_TARGET := x86_64-unknown-none
RUST_LIB := rust/target/$(RUST_TARGET)/release/libmagia_rs.a

GCC_INCLUDE := $(shell $(CC) -print-file-name=include)

CFLAGS := -std=gnu11 -O2 -g \
	-ffreestanding -nostdinc -isystem $(GCC_INCLUDE) -Iinclude \
	-fno-pic -fno-pie -mcmodel=kernel \
	-mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mgeneral-regs-only \
	-fstack-protector-strong -mstack-protector-guard=global -ftrivial-auto-var-init=zero \
	-fno-omit-frame-pointer -fno-asynchronous-unwind-tables \
	-fno-strict-aliasing -fno-common -ffunction-sections -fdata-sections \
	-fcf-protection=none -U_FORTIFY_SOURCE \
	-Wall -Wextra -Werror -Wno-unused-parameter -Wstrict-prototypes -Wmissing-prototypes \
	-MMD -MP
ASFLAGS := -Iinclude -D__ASSEMBLY__ -g
LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack \
	--gc-sections --no-warn-rwx-segments -T arch/x86_64/linker.ld

C_SRCS   := $(wildcard kernel/*.c) $(wildcard kernel/ktest/*.c)
ASM_SRCS := $(wildcard arch/x86_64/*.S)
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS)) $(patsubst %.S,$(BUILD)/%.o,$(ASM_SRCS))
RUST_SRCS := $(shell find rust/src -name '*.rs') rust/Cargo.toml

.PHONY: all run test check cargo-test ktest clean lines

all: $(BUILD)/magia.elf $(BUILD)/magia32.elf

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(RUST_LIB): $(RUST_SRCS)
	cd rust && $(CARGO) build --release --target $(RUST_TARGET)

# Two-pass link: pass 1 without symbols, then embed a sorted symbol table
# (for panic backtraces) in .ksyms at the end of .rodata. Code addresses do
# not move between passes because .ksyms comes after .text.
$(BUILD)/ksyms_empty.o: scripts/gen-ksyms.sh
	@mkdir -p $(BUILD)
	sh scripts/gen-ksyms.sh /dev/null > $(BUILD)/ksyms_empty.S
	$(CC) $(ASFLAGS) -c $(BUILD)/ksyms_empty.S -o $@

$(BUILD)/magia.pass1.elf: $(OBJS) $(RUST_LIB) $(BUILD)/ksyms_empty.o arch/x86_64/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(BUILD)/ksyms_empty.o $(RUST_LIB)

$(BUILD)/ksyms.o: $(BUILD)/magia.pass1.elf scripts/gen-ksyms.sh
	$(NM) -n $< | sh scripts/gen-ksyms.sh - > $(BUILD)/ksyms.S
	$(CC) $(ASFLAGS) -c $(BUILD)/ksyms.S -o $@

$(BUILD)/magia.elf: $(OBJS) $(RUST_LIB) $(BUILD)/ksyms.o
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(BUILD)/ksyms.o $(RUST_LIB)
	@sh scripts/check-text-stable.sh $(BUILD)/magia.pass1.elf $@

# QEMU's multiboot loader only accepts 32-bit ELF images.
$(BUILD)/magia32.elf: $(BUILD)/magia.elf
	$(OBJCOPY) -O elf32-i386 $< $@

QEMU_COMMON := -machine pc -cpu max -accel $(QEMU_ACCEL) -m $(QEMU_MEM) -no-reboot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04

run: all
	$(QEMU) $(QEMU_COMMON) -kernel $(BUILD)/magia32.elf -nographic

cargo-test:
	cd rust && $(CARGO) test --target x86_64-unknown-linux-gnu

ktest: all
	QEMU="$(QEMU)" QEMU_ACCEL="$(QEMU_ACCEL)" QEMU_MEM="$(QEMU_MEM)" sh tests/run-ktest.sh $(BUILD)/magia32.elf

test: cargo-test ktest
	@echo "make test: all tests passed"

check: test

lines:
	@sh scripts/linecount.sh

clean:
	rm -rf $(BUILD) rust/target

-include $(OBJS:.o=.d)
