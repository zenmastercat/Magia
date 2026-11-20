/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * Magia kernel linker script (Limine boot protocol, static PIE).
 *  - linked at KERNEL_LINK_BASE (-2 GiB) as a position-independent
 *    executable; Limine applies the R_X86_64_RELATIVE relocations and,
 *    with "kaslr: yes", slides the whole image by a random 4 KiB multiple
 *    (kernel/boot.c records the slide).
 *  - one PT_LOAD per permission: text R+X, rodata R, data R+W, so no
 *    segment is RWX; vmm.c re-applies the same W^X permissions per section.
 *  - each output section starts on a page boundary.
 *  - linker sets (set_*) are gathered and sorted inside .rodata; the
 *    backtrace symbol table (.ksyms) and the dynamic relocation tables come
 *    after everything else in the read-only segment, so the second link
 *    pass (which adds .ksyms and its relocations) never moves code.
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_LINK_BASE = 0xffffffff80000000;

PHDRS
{
	text    PT_LOAD FLAGS(5);
	rodata  PT_LOAD FLAGS(4);
	data    PT_LOAD FLAGS(6);
	dynamic PT_DYNAMIC FLAGS(4);
}

SECTIONS
{
	. = KERNEL_LINK_BASE;
	__kernel_start = .;

	.text : {
		__text_start = .;
		*(.text.entry)
		*(.text .text.*)
		. = ALIGN(16);
		__user_blob_section = .;
		KEEP(*(.user_blob))
		. = ALIGN(4096);
		__text_end = .;
	} :text

	.rodata : ALIGN(4096) {
		__rodata_start = .;
		*(.rodata .rodata.*)
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
	} :rodata
	/* written by Limine's relocation pass before entry, read-only after */
	.data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :rodata
	.got : { *(.got) *(.igot) } :rodata
	.got.plt : { *(.got.plt) *(.igot.plt) } :rodata
	.ksyms : { KEEP(*(.ksyms)) } :rodata
	.dynamic : { *(.dynamic) } :rodata :dynamic
	.dynsym : { *(.dynsym) } :rodata
	.dynstr : { *(.dynstr) } :rodata
	.hash : { *(.hash) } :rodata
	.gnu.hash : { *(.gnu.hash) } :rodata
	.rela.dyn : { *(.rela.*) } :rodata
	.relr.dyn : { *(.relr.dyn) } :rodata
	. = ALIGN(4096);
	__rodata_end = .;

	.data : ALIGN(4096) {
		__data_start = .;
		/* Limine request markers and requests (kernel/boot.c) */
		KEEP(*(.limine_requests_start))
		KEEP(*(.limine_requests))
		KEEP(*(.limine_requests_end))
		*(.data .data.*)
	} :data

	.bss : {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(4096);
		__bss_end = .;
	} :data

	__kernel_end = .;

	/DISCARD/ : {
		*(.eh_frame .eh_frame_hdr .eh_frame.*)
		*(.note .note.*)
		*(.comment)
		*(.interp)
	}
}
