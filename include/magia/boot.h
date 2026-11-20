/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * boot.h - what the bootloader told us, copied out of Limine's
 * bootloader-reclaimable memory by kernel/boot.c before anything else runs,
 * so that memory can be given to the page allocator later.
 */
#ifndef MAGIA_BOOT_H
#define MAGIA_BOOT_H

#include <magia/kernel.h>

/* Memory map types (same values as LIMINE_MEMMAP_*). */
#define MEM_USABLE      0
#define MEM_RESERVED    1
#define MEM_ACPI_RECLAIM 2
#define MEM_ACPI_NVS    3
#define MEM_BAD         4
#define MEM_BOOT_RECLAIM 5
#define MEM_KERNEL      6 /* executable and modules */
#define MEM_FRAMEBUFFER 7
#define MEM_RESERVED_MAPPED 8 /* firmware data Limine maps (SMBIOS, EFI runtime) */

#define BOOT_MMAP_MAX 512

struct boot_mmap_entry {
	uint64_t base, length, type;
};

struct boot_fb {
	uint64_t pa;     /* 0: no framebuffer */
	uint64_t width, height, pitch;
	uint16_t bpp;
	uint8_t red_shift, green_shift, blue_shift;
	uint8_t red_size, green_size, blue_size;
};

#define BOOT_FW_BIOS  0
#define BOOT_FW_EFI32 1
#define BOOT_FW_EFI64 2

#define BOOT_ENTROPY_MAX 8

struct bootinfo {
	uint64_t base_revision;   /* Limine base revision actually used */
	uint64_t kernel_phys;     /* physical load address of the image */
	uint64_t kernel_virt;     /* virtual load address (= link base + slide) */
	uint64_t kernel_slide;    /* KASLR slide, a multiple of 4 KiB */
	uint64_t firmware;        /* BOOT_FW_* */
	uint64_t rsdp_pa;         /* 0 if the firmware has no ACPI */
	size_t nmmap;
	struct boot_mmap_entry mmap[BOOT_MMAP_MAX];
	struct boot_fb fb;
	size_t nentropy;
	uint64_t entropy[BOOT_ENTROPY_MAX]; /* wiped once the RNG has them */
	char cmdline[256];
};
extern struct bootinfo bootinfo;

void boot_early(void); /* called from _start before kmain */

#endif
