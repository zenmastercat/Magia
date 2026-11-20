/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * Magia kernel linker script.
 *  - .boot* sections run at their physical address (1 MiB), before paging;
 *    code (R+X) and data (R+W) get separate program headers, so no
 *    segment in the ELF is RWX.
 *  - everything else is linked at KERNEL_VMA + physical (higher half).
 *  - each output section starts on a page boundary so vmm.c can apply
 *    W^X permissions per section (text RX, rodata R, data/bss RW+NX).
 *  - linker sets (set_*) are gathered and sorted inside .rodata.
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_VMA = 0xffffffff80000000;

PHDRS
{
	boottext PT_LOAD FLAGS(5); /* R+X: multiboot header, 32-bit entry  */
	bootdata PT_LOAD FLAGS(6); /* R+W: boot page tables, boot stack    */
	text   PT_LOAD FLAGS(5);
	rodata PT_LOAD FLAGS(4);
	data   PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = 0x100000;
	__kernel_phys_start = .;
	.boot : {
		KEEP(*(.multiboot))
		*(.boot.text)
	} :boottext

	. = ALIGN(4096);
	.bootdata : {
		*(.boot.data)
	} :bootdata

	. = ALIGN(4096);
	. += KERNEL_VMA;

	.text : AT(ADDR(.text) - KERNEL_VMA) {
		__text_start = .;
		*(.text .text.*)
		. = ALIGN(16);
		__user_blob_section = .;
		KEEP(*(.user_blob))
		. = ALIGN(4096);
		__text_end = .;
	} :text

	.rodata : AT(ADDR(.rodata) - KERNEL_VMA) {
		__rodata_start = .;
		*(.rodata .rodata.*)
		*(.data.rel.ro .data.rel.ro.*)
		*(.got .got.plt)
		. = ALIGN(8);
		__start_set_sysinit = .;
		KEEP(*(SORT(set_sysinit.*)))
		__stop_set_sysinit = .;
		__start_set_ktest = .;
		KEEP(*(SORT(set_ktest.*)))
		__stop_set_ktest = .;
		__start_set_sysctl = .;
		KEEP(*(SORT(set_sysctl.*)))
		__stop_set_sysctl = .;
		/* the symbol table for backtraces comes last so that adding it
		 * in the second link pass does not move any code */
		KEEP(*(.ksyms))
		. = ALIGN(4096);
		__rodata_end = .;
	} :rodata

	.data : AT(ADDR(.data) - KERNEL_VMA) {
		__data_start = .;
		*(.data .data.*)
	} :data

	.bss : AT(ADDR(.bss) - KERNEL_VMA) {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(4096);
		__bss_end = .;
	} :data

	__kernel_end = .;
	__kernel_phys_end = . - KERNEL_VMA;

	/DISCARD/ : {
		*(.eh_frame .eh_frame_hdr .eh_frame.*)
		*(.note .note.*)
		*(.comment)
		*(.interp .dynsym .dynstr .dynamic .hash .gnu.hash)
	}
}
