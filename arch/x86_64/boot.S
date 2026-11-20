/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * boot.S - multiboot1 entry, 32-bit protected mode -> 64-bit long mode.
 *
 * QEMU's -kernel loader (or any multiboot1 loader) jumps to _start in
 * 32-bit protected mode with paging off, EAX = 0x2BADB002, EBX = physical
 * address of the multiboot info. We build throw-away page tables that map
 * the first 1 GiB three times (identity, direct map at PHYS_MAP_BASE, and
 * the kernel window at KERNEL_VMA), enable PAE + long mode (+ NX if the CPU
 * has it), jump to 64-bit code, then to the higher half and call kmain().
 * kernel/vmm.c replaces these tables with fine-grained W^X ones.
 */

#define KERNEL_VMA 0xffffffff80000000
#define MB_MAGIC   0x1BADB002
#define MB_FLAGS   0x00000003 /* page-align modules, provide memory map */

	.section .multiboot, "a"
	.align 4
	.long MB_MAGIC
	.long MB_FLAGS
	.long -(MB_MAGIC + MB_FLAGS)

	.section .boot.text, "ax"
	.code32
	.global _start
_start:
	cli
	cld
	movl $boot_stack_top, %esp
	movl %eax, boot_mb_magic
	movl %ebx, boot_mb_info

	/* Long mode available? */
	movl $0x80000000, %eax
	cpuid
	cmpl $0x80000001, %eax
	jb no_long_mode
	movl $0x80000001, %eax
	cpuid
	testl $(1 << 29), %edx
	jz no_long_mode
	movl %edx, %ebp /* remember bit 20 (NX) */

	/* Zero the four boot page-table pages. */
	movl $boot_pml4, %edi
	movl $(4096 * 4 / 4), %ecx
	xorl %eax, %eax
	rep stosl

	/* PML4[0] and PML4[256] -> pdpt_lo, PML4[511] -> pdpt_hi */
	movl $boot_pdpt_lo, %eax
	orl $0x3, %eax
	movl %eax, boot_pml4
	movl %eax, boot_pml4 + 256 * 8
	movl $boot_pdpt_hi, %eax
	orl $0x3, %eax
	movl %eax, boot_pml4 + 511 * 8
	/* pdpt_lo[0] -> pd, pdpt_hi[510] -> pd (0xffffffff80000000) */
	movl $boot_pd, %eax
	orl $0x3, %eax
	movl %eax, boot_pdpt_lo
	movl %eax, boot_pdpt_hi + 510 * 8
	/* pd[i] = i * 2 MiB | PS | W | P */
	movl $boot_pd, %edi
	xorl %ecx, %ecx
1:	movl %ecx, %eax
	shll $21, %eax
	orl $0x83, %eax
	movl %eax, (%edi, %ecx, 8)
	incl %ecx
	cmpl $512, %ecx
	jne 1b

	/* CR4.PAE | CR4.PGE */
	movl %cr4, %eax
	orl $((1 << 5) | (1 << 7)), %eax
	movl %eax, %cr4
	movl $boot_pml4, %eax
	movl %eax, %cr3

	/* EFER.LME, plus EFER.NXE when supported */
	movl $0xc0000080, %ecx
	rdmsr
	orl $(1 << 8), %eax
	testl $(1 << 20), %ebp
	jz 2f
	orl $(1 << 11), %eax
2:	wrmsr

	/* CR0.PG | CR0.WP | CR0.PE */
	movl %cr0, %eax
	orl $((1 << 31) | (1 << 16) | 1), %eax
	movl %eax, %cr0

	lgdt boot_gdtr
	ljmp $0x08, $long_entry

no_long_mode:
	movl $msg_nolm, %esi
3:	lodsb
	testb %al, %al
	jz 4f
	movw $0x3f8, %dx
	outb %al, %dx
	jmp 3b
4:	hlt
	jmp 4b

	.code64
long_entry:
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss
	xorw %ax, %ax
	movw %ax, %fs
	movw %ax, %gs
	movabsq $higher_half, %rax
	jmp *%rax

	.section .boot.data, "aw"
	.align 16
boot_gdt:
	.quad 0
	.quad 0x00af9a000000ffff /* 64-bit code */
	.quad 0x00cf92000000ffff /* data */
boot_gdt_end:
boot_gdtr:
	.word boot_gdt_end - boot_gdt - 1
	.long boot_gdt
	.global boot_mb_magic, boot_mb_info
boot_mb_magic: .long 0
boot_mb_info:  .long 0
msg_nolm: .asciz "Magia: this CPU does not support x86_64 long mode\r\n"

	.align 4096
	.global boot_pml4
boot_pml4:    .skip 4096
boot_pdpt_lo: .skip 4096
boot_pdpt_hi: .skip 4096
boot_pd:      .skip 4096
boot_stack:   .skip 4096
boot_stack_top:

/* ------------------------------------------------------------------ */
	.section .text
	.code64
higher_half:
	movabsq $boot_kstack_top, %rsp
	xorl %ebp, %ebp

	/* Seed the stack-protector guard before any C code runs: RDRAND if
	 * CPUID.1:ECX[30] says it exists, mixed with the TSC. */
	movl $1, %eax
	cpuid
	rdtsc
	shlq $32, %rdx
	orq %rax, %rdx
	movq %rdx, %r8
	movl $1, %eax
	cpuid
	testl $(1 << 30), %ecx
	jz 6f
	movl $16, %ecx
5:	rdrand %rax
	jc 7f
	loop 5b
	jmp 6f
7:	xorq %rax, %r8
6:	andq $-256, %r8 /* low byte 0: string overreads stop at the canary */
	movabsq $__stack_chk_guard, %rax
	movq %r8, (%rax)

	movl boot_mb_magic, %edi
	movl boot_mb_info, %esi
	call kmain
8:	cli
	hlt
	jmp 8b

	.section .bss
	.align 16
	.global boot_kstack, boot_kstack_top
boot_kstack:
	.skip 16384
boot_kstack_top:
