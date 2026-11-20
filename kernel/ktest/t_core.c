/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* t_core.c - ktests for boot, CPU setup, traps and linker sets (0xx). */
#include <magia/cpu.h>
#include <magia/ktest.h>
#include <magia/sysctl.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

KTEST(001, boot_cmdline_and_sysinit)
{
	KT_ASSERT(cmdline_has("ktest"));
	KT_ASSERT(!cmdline_has("ktes"));     /* whole words only */
	KT_ASSERT(MAGIA_SET_COUNT(sysinit) >= 8);
	KT_ASSERT(MAGIA_SET_COUNT(ktest) >= 10);
}

/* SYSINIT runs in key order; check the set is sorted by walking names. */
KTEST(002, linker_set_sysinit_order)
{
	const struct sysinit *const *si;
	int pos_cpu = -1, pos_pmm = -1, pos_vmm = -1, pos_rust = -1, i = 0;
	MAGIA_SET_FOREACH(si, sysinit) {
		if (!strcmp((*si)->name, "cpu"))
			pos_cpu = i;
		if (!strcmp((*si)->name, "pmm"))
			pos_pmm = i;
		if (!strcmp((*si)->name, "vmm"))
			pos_vmm = i;
		if (!strcmp((*si)->name, "rust"))
			pos_rust = i;
		i++;
	}
	KT_ASSERT(pos_cpu >= 0 && pos_pmm > pos_cpu && pos_vmm > pos_pmm && pos_rust > pos_vmm);
}

KTEST(003, linker_set_sysctl_nodes)
{
	const struct c_sysctl *const *sp;
	bool ticks_found = false;
	MAGIA_SET_FOREACH(sp, sysctl) {
		KT_ASSERT((*sp)->name != NULL && strchr((*sp)->name, '.') != NULL);
		if (!strcmp((*sp)->name, "kern.ticks"))
			ticks_found = true;
	}
	KT_ASSERT(ticks_found);
}

KTEST(010, breakpoint_exception_returns)
{
	uint64_t before = breakpoint_hits;
	__asm__ volatile("int3");
	__asm__ volatile("int3");
	KT_EQ(breakpoint_hits - before, 2);
}

KTEST(011, cpu_protection_bits)
{
	KT_ASSERT(read_cr0() & CR0_WP);
	KT_EQ(!!(rdmsr(MSR_EFER) & EFER_NXE), cpu_feat.nx);
	KT_EQ(!!(read_cr4() & CR4_SMEP), cpu_feat.smep);
	KT_EQ(!!(read_cr4() & CR4_SMAP), cpu_feat.smap);
	KT_ASSERT(rdmsr(MSR_EFER) & EFER_SCE);
	KT_ASSERT((read_rflags() & RFLAGS_AC) == 0); /* SMAP is enforced */
}

KTEST(012, timer_ticks_and_tsc)
{
	uint64_t t0 = ticks, n0 = now_ns();
	thread_sleep_ms(20);
	uint64_t dt = ticks - t0, dn = now_ns() - n0;
	KT_ASSERT(dt >= 20 && dt < 200);
	KT_ASSERT(dn >= 15000000ULL && dn < 400000000ULL);
	KT_ASSERT(tsc_hz() > 1000000);
}

KTEST(013, gdt_tss_syscall_msrs)
{
	uint64_t star = rdmsr(MSR_STAR);
	KT_EQ((star >> 32) & 0xffff, GDT_KCODE);
	KT_EQ((star >> 48) & 0xffff, GDT_UBASE);
	KT_ASSERT(rdmsr(MSR_LSTAR) >= KERNEL_VMA);
	KT_ASSERT(rdmsr(MSR_SFMASK) & RFLAGS_IF);
	KT_ASSERT(rdmsr(MSR_SFMASK) & RFLAGS_AC);
	KT_EQ(rdmsr(MSR_GS_BASE), (uint64_t)&cpu0);
	uint16_t tr;
	__asm__ volatile("str %0" : "=r"(tr));
	KT_EQ(tr, GDT_TSS);
}

KTEST(014, ksym_lookup_resolves_kernel_symbols)
{
	uint64_t off = 1;
	const char *n = ksym_lookup((uint64_t)ktest_run_all, &off);
	KT_ASSERT(n != NULL);
	KT_ASSERT(n && !strcmp(n, "ktest_run_all"));
	KT_EQ(off, 0);
	KT_ASSERT(ksym_lookup(0x1234, NULL) == NULL); /* negative: not kernel text */
}

KTEST(015, printf_formats)
{
	char b[64];
	ksnprintf(b, sizeof(b), "%d|%5u|%-3x|%05ld|%s|%.2s|%c", -42, 7u, 0xabu, 123L, "ok", "xyz", '!');
	KT_ASSERT(!strcmp(b, "-42|    7|ab |00123|ok|xy|!"));
	int n = ksnprintf(b, 4, "%s", "truncated");
	KT_EQ(n, 9);
	KT_ASSERT(!strcmp(b, "tru"));
	bool ok;
	KT_EQ(katol("0x1f", &ok), 31);
	KT_ASSERT(ok);
	katol("12z", &ok);
	KT_ASSERT(!ok);
}
