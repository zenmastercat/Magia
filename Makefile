# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 zenmastercat and Magia contributors
#
# Magia build.
#   make          build build/magia.elf (static PIE for the Limine boot protocol)
#   make image    build/magia.img: GPT disk, FAT32 ESP, Limine for UEFI and BIOS
#   make run      boot the image in QEMU with OVMF, serial console on stdio
#                 (CMDLINE="..." sets the kernel command line)
#   make test     host cargo tests + headless QEMU runs through OVMF (tests/run-ktest.sh)
#   make check    alias for test

CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy
NM      ?= nm
CARGO   ?= cargo
HOSTCC  ?= gcc
QEMU    ?= qemu-system-x86_64
QEMU_ACCEL ?= tcg
QEMU_MEM   ?= 8G
QEMU_SMP   ?= 4
# OVMF (UEFI firmware for QEMU); shell.nix exports OVMF_FD.
OVMF    ?= $(OVMF_FD)
CMDLINE ?=

# GNU make defaults CC to "cc"; insist on gcc for the kernel flags below.
ifeq ($(origin CC),default)
CC := gcc
endif

BUILD := build
RUST_TARGET := x86_64-unknown-none
RUST_LIB := rust/target/$(RUST_TARGET)/release/libmagia_rs.a

GCC_INCLUDE := $(shell $(CC) -print-file-name=include)

# -fpie: the kernel is a static PIE (RIP-relative code, R_X86_64_RELATIVE
# data relocations applied by Limine), so the image can be slid by KASLR.
CFLAGS := -std=gnu11 -O2 -g \
	-ffreestanding -nostdinc -isystem $(GCC_INCLUDE) -Iinclude \
	-fpie -fno-plt \
	-mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mgeneral-regs-only \
	-fstack-protector-strong -mstack-protector-guard=global -ftrivial-auto-var-init=zero \
	-fno-omit-frame-pointer -fno-asynchronous-unwind-tables \
	-fno-strict-aliasing -fno-common -ffunction-sections -fdata-sections \
	-fcf-protection=none -U_FORTIFY_SOURCE \
	-Wall -Wextra -Werror -Wno-unused-parameter -Wstrict-prototypes -Wmissing-prototypes \
	-MMD -MP
ASFLAGS := -Iinclude -D__ASSEMBLY__ -g
# -z text: refuse text relocations, i.e. any absolute address in code.
LDFLAGS := -nostdlib -pie --no-dynamic-linker -z text -z max-page-size=0x1000 \
	-z noexecstack --gc-sections -T arch/x86_64/linker.ld

C_SRCS   := $(wildcard kernel/*.c) $(wildcard kernel/ktest/*.c)
ASM_SRCS := $(wildcard arch/x86_64/*.S)
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS)) $(patsubst %.S,$(BUILD)/%.o,$(ASM_SRCS))
RUST_SRCS := $(shell find rust/src -name '*.rs') rust/Cargo.toml rust/.cargo/config.toml

.PHONY: all image run test check cargo-test ktest limine clean lines

all: $(BUILD)/magia.elf

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(RUST_LIB): $(RUST_SRCS)
	cd rust && $(CARGO) build --release --target $(RUST_TARGET)

# Two-pass link: pass 1 without symbols, then embed a sorted symbol table
# (for panic backtraces) in .ksyms near the end of the read-only segment.
# Code addresses do not move between passes because .ksyms comes after
# .text and .rodata.
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
	@sh scripts/fix-pie.sh $@

# ---------------------------------------------------------------- Limine
# Pinned release binaries (BSD-2-Clause; 3RDPARTY.md lists their parts),
# fetched once and checked against the SHA-256 of the v12.10.0 release
# asset. Set LIMINE_TARBALL=/path/limine-binary.tar.gz to build offline.
LIMINE_VERSION := 12.10.0
LIMINE_SHA256  := 365ba8785d4e4b55c3368738a642681b31f606afec9f77179e07fc26ebe841e2
LIMINE_URL     := https://github.com/Limine-Bootloader/Limine/releases/download/v$(LIMINE_VERSION)/limine-binary.tar.gz
LIMINE_DIR     := $(BUILD)/limine-$(LIMINE_VERSION)
LIMINE_TARBALL ?= $(BUILD)/dl/limine-binary-$(LIMINE_VERSION).tar.gz

$(BUILD)/dl/limine-binary-$(LIMINE_VERSION).tar.gz:
	@mkdir -p $(dir $@)
	curl -fsSL --retry 3 -o $@.part $(LIMINE_URL)
	mv $@.part $@

$(LIMINE_DIR)/.ok: $(LIMINE_TARBALL)
	echo "$(LIMINE_SHA256)  $(LIMINE_TARBALL)" | sha256sum -c -
	rm -rf $(LIMINE_DIR) && mkdir -p $(LIMINE_DIR)
	tar -xzf $(LIMINE_TARBALL) -C $(LIMINE_DIR) --strip-components=1
	$(HOSTCC) -O2 -std=c99 -D_FILE_OFFSET_BITS=64 $(LIMINE_DIR)/limine.c -o $(LIMINE_DIR)/limine
	touch $@

limine: $(LIMINE_DIR)/.ok

# ---------------------------------------------------------------- image
# 64 MiB GPT disk: one 62 MiB FAT32 EFI system partition at 1 MiB holding
# Limine (UEFI: EFI/BOOT/BOOTX64.EFI; BIOS: limine-bios.sys plus the boot
# sector code that `limine bios-install` writes), its config and the
# kernel. Built from plain files with mtools and sgdisk: no root, no loop
# devices, nothing but build/ is written.
IMG := $(BUILD)/magia.img

$(IMG): $(BUILD)/magia.elf $(LIMINE_DIR)/.ok boot/limine.conf
	rm -f $@.tmp $(BUILD)/esp.img
	truncate -s 64M $@.tmp
	sgdisk -o -n 1:2048:+62M -t 1:ef00 -c 1:MAGIA-ESP \
		-a 1 -n 2:34:2047 -t 2:ef02 -c 2:LIMINE-BIOS $@.tmp >/dev/null
	truncate -s 62M $(BUILD)/esp.img
	mformat -i $(BUILD)/esp.img -F -c 1 -v MAGIA ::
	mmd -i $(BUILD)/esp.img ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $(BUILD)/esp.img $(LIMINE_DIR)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $(BUILD)/esp.img $(LIMINE_DIR)/limine-bios.sys boot/limine.conf \
		$(LIMINE_DIR)/LICENSE $(LIMINE_DIR)/3RDPARTY.md ::/boot/limine/
	mcopy -i $(BUILD)/esp.img $(BUILD)/magia.elf ::/boot/magia.elf
	dd if=$(BUILD)/esp.img of=$@.tmp bs=1M seek=1 conv=notrunc status=none
	$(LIMINE_DIR)/limine bios-install --quiet $@.tmp
	rm -f $(BUILD)/esp.img
	mv $@.tmp $@
	@echo "image: $@ (GPT, FAT32 ESP, Limine $(LIMINE_VERSION) for UEFI and BIOS)"

image: $(IMG)

# ---------------------------------------------------------------- QEMU
QEMU_ENV = QEMU="$(QEMU)" QEMU_ACCEL="$(QEMU_ACCEL)" QEMU_MEM="$(QEMU_MEM)" QEMU_SMP="$(QEMU_SMP)" OVMF="$(OVMF)"

run: $(IMG)
	$(QEMU_ENV) sh tests/qemu.sh $(IMG) "$(CMDLINE)" -serial mon:stdio

cargo-test:
	cd rust && $(CARGO) test --target x86_64-unknown-linux-gnu

ktest: $(IMG)
	$(QEMU_ENV) sh tests/run-ktest.sh $(IMG)

test: cargo-test ktest
	@echo "make test: all tests passed"

check: test

lines:
	@sh scripts/linecount.sh

clean:
	rm -rf $(BUILD) rust/target

-include $(OBJS:.o=.d)
