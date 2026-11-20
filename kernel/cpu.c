/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * cpu.c - GDT, TSS, per-CPU data, CPU feature detection and the
 * protection bits: CR0.WP, EFER.NXE, CR4.SMEP, CR4.SMAP, CR4.UMIP.
 */
#include <magia/cpu.h>
#include <magia/linker_set.h>
#include <magia/x86.h>

struct percpu cpu0;
struct cpu_features cpu_feat;
bool smap_enabled;
bool smep_enabled;
bool umip_enabled;

struct __packed tss64 {
	uint32_t _r0;
	uint64_t rsp[3];
	uint64_t _r1;
	uint64_t ist[7];
	uint64_t _r2;
	uint16_t _r3;
	uint16_t iomap_base;
};

static struct tss64 tss __aligned(16);
static uint8_t df_stack[8192] __aligned(16);  /* IST1: double fault */
static uint8_t nmi_stack[4096] __aligned(16); /* IST2: NMI           */

static uint64_t gdt[8] __aligned(16) = {
	0,
	0x00af9a000000ffffULL, /* 0x08 kernel code, 64-bit          */
	0x00cf92000000ffffULL, /* 0x10 kernel data                  */
	0x00cffa000000ffffULL, /* 0x18 user code 32-bit (sysret base, unused) */
	0x00cff2000000ffffULL, /* 0x20 user data, DPL 3             */
	0x00affa000000ffffULL, /* 0x28 user code 64-bit, DPL 3      */
	0, 0,                  /* 0x30 TSS (16-byte descriptor)     */
};

void gdt_init(void)
{
	uint64_t base = (uint64_t)&tss;
	uint64_t limit = sizeof(tss) - 1;
	tss.ist[0] = (uint64_t)(df_stack + sizeof(df_stack));
	tss.ist[1] = (uint64_t)(nmi_stack + sizeof(nmi_stack));
	tss.iomap_base = sizeof(tss); /* no I/O bitmap: ring 3 port I/O faults */
	gdt[6] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ULL << 40) |
		 (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
	gdt[7] = base >> 32;

	struct __packed {
		uint16_t limit;
		uint64_t base;
	} gdtr = { sizeof(gdt) - 1, (uint64_t)gdt };
	__asm__ volatile("lgdt %0\n\t"
			 "pushq $0x08\n\t"
			 "leaq 1f(%%rip), %%rax\n\t"
			 "pushq %%rax\n\t"
			 "lretq\n"
			 "1:\n\t"
			 "movw $0x10, %%ax\n\t"
			 "movw %%ax, %%ds\n\t"
			 "movw %%ax, %%es\n\t"
			 "movw %%ax, %%ss\n\t"
			 "xorw %%ax, %%ax\n\t"
			 "movw %%ax, %%fs\n\t"
			 "movw %%ax, %%gs\n\t"
			 :
			 : "m"(gdtr)
			 : "rax", "memory");
	__asm__ volatile("ltr %w0" ::"r"((uint16_t)GDT_TSS));

	cpu0.self = (uint64_t)&cpu0;
	wrmsr(MSR_GS_BASE, (uint64_t)&cpu0);
	wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void tss_set_rsp0(uint64_t rsp0)
{
	tss.rsp[0] = rsp0;
	cpu0.kstack_top = rsp0;
}

static void detect_features(void)
{
	uint32_t a, b, c, d, max, maxext;
	cpuid(0, 0, &max, &b, &c, &d);
	memcpy(cpu_feat.vendor + 0, &b, 4);
	memcpy(cpu_feat.vendor + 4, &d, 4);
	memcpy(cpu_feat.vendor + 8, &c, 4);
	cpu_feat.vendor[12] = '\0';
	cpuid(1, 0, &a, &b, &c, &d);
	cpu_feat.rdrand = (c >> 30) & 1;
	if (max >= 7) {
		cpuid(7, 0, &a, &b, &c, &d);
		cpu_feat.smep = (b >> 7) & 1;
		cpu_feat.smap = (b >> 20) & 1;
		umip_enabled = (c >> 2) & 1; /* provisional: "available" */
	}
	cpuid(0x80000000, 0, &maxext, &b, &c, &d);
	if (maxext >= 0x80000001) {
		cpuid(0x80000001, 0, &a, &b, &c, &d);
		cpu_feat.nx = (d >> 20) & 1;
		cpu_feat.pdpe1gb = (d >> 26) & 1;
	}
	if (maxext >= 0x80000004) {
		uint32_t *p = (uint32_t *)cpu_feat.brand;
		for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++, p += 4)
			cpuid(leaf, 0, &p[0], &p[1], &p[2], &p[3]);
		cpu_feat.brand[48] = '\0';
	}
	if (maxext >= 0x80000007) {
		cpuid(0x80000007, 0, &a, &b, &c, &d);
		cpu_feat.invariant_tsc = (d >> 8) & 1;
	}
}

void cpu_init(void)
{
	detect_features();
	write_cr0(read_cr0() | CR0_WP); /* kernel writes honour read-only PTEs */
	uint64_t efer = rdmsr(MSR_EFER);
	if (cpu_feat.nx)
		efer |= EFER_NXE;
	wrmsr(MSR_EFER, efer);
	uint64_t cr4 = read_cr4();
	if (cpu_feat.smep) {
		cr4 |= CR4_SMEP;
		smep_enabled = true;
	}
	if (cpu_feat.smap) {
		cr4 |= CR4_SMAP;
		smap_enabled = true;
	}
	if (umip_enabled)
		cr4 |= 1ULL << 11; /* UMIP: ring 3 may not run sgdt/sidt/str */
	write_cr4(cr4);
	/* make sure AC starts clear so SMAP is enforced */
	__asm__ volatile("pushfq; andq $~(1 << 18), (%%rsp); popfq" ::: "memory", "cc");
}

void cpu_report(void)
{
	const char *b = cpu_feat.brand;
	while (*b == ' ')
		b++;
	kprintf("cpu: %s \"%s\"\n", cpu_feat.vendor, b);
	kprintf("cpu: NX %s, WP on, SMEP %s, SMAP %s, UMIP %s, RDRAND %s, invariant TSC %s\n",
		cpu_feat.nx ? "on" : "unavailable", smep_enabled ? "on" : "unavailable",
		smap_enabled ? "on" : "unavailable", umip_enabled ? "on" : "unavailable",
		cpu_feat.rdrand ? "yes" : "no", cpu_feat.invariant_tsc ? "yes" : "no");
}

static void cpu_sysinit(void)
{
	gdt_init();
	idt_init();
	cpu_init();
}
SYSINIT(cpu, SI_ORDER_EARLY, cpu_sysinit);
