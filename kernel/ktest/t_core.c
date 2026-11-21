/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* t_core.c - ktests for boot, CPU setup, traps and linker sets (0xx). */
#include <magia/acpi.h>
#include <magia/boot.h>
#include <magia/cpu.h>
#include <magia/ktest.h>
#include <magia/mm.h>
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

/* Limine hand-off: KASLR slide, randomised direct map, symbols under the slide. */
KTEST(004, boot_limine_kaslr_and_direct_map)
{
	extern char __text_start[], __kernel_start[];
	KT_EQ(bootinfo.base_revision, 6);
	KT_ASSERT(bootinfo.firmware == BOOT_FW_EFI64 || bootinfo.firmware == BOOT_FW_BIOS);
	KT_ASSERT(kernel_virt_base >= KERNEL_VMA);
	KT_EQ(kernel_virt_base & (PAGE_SIZE - 1), 0);
	KT_EQ(kernel_virt_base, (uint64_t)__kernel_start); /* the image runs where Limine said */
	KT_EQ(bootinfo.kernel_slide, kernel_virt_base - KERNEL_VMA);
	KT_EQ(kernel_phys_base & (PAGE_SIZE - 1), 0);
	/* direct map: in the higher half, below the image, translating to itself */
	KT_ASSERT(dmap_base >= 0xffff800000000000ULL && dmap_base < KERNEL_VMA);
	uint64_t pa = 0;
	KT_ASSERT(vmm_translate((uint64_t)phys_to_virt(kernel_phys_base), &pa, NULL));
	KT_EQ(pa, kernel_phys_base);
	KT_ASSERT(vmm_translate((uint64_t)__text_start, &pa, NULL));
	KT_EQ(pa, virt_to_phys(__text_start));
	/* the symbol table stores link-time addresses: lookups subtract the slide */
	uint64_t off = 1;
	const char *name = ksym_lookup((uint64_t)ktest_run_all, &off);
	KT_ASSERT(name && strcmp(name, "ktest_run_all") == 0);
	KT_EQ(off, 0);
	/* the boot copy of Limine's data is complete */
	KT_ASSERT(bootinfo.nmmap > 0 && bootinfo.rsdp_pa != 0);
	KT_ASSERT(strcmp(kernel_cmdline, bootinfo.cmdline) == 0);
}

/* The Rust parser read QEMU's q35 tables (-smp 4): checked against what
 * QEMU builds, so a parser regression shows up here and in cargo test. */
KTEST(020, acpi_tables_match_qemu)
{
	KT_EQ(acpi_status, 0);
	KT_EQ(acpi.found & (ACPI_FOUND_MADT | ACPI_FOUND_HPET | ACPI_FOUND_MCFG | ACPI_FOUND_FADT),
	      ACPI_FOUND_MADT | ACPI_FOUND_HPET | ACPI_FOUND_MCFG | ACPI_FOUND_FADT);
	KT_EQ(acpi.ncpus, 4);
	for (uint32_t i = 0; i < acpi.ncpus && i < 4; i++)
		KT_EQ(acpi.cpu_apic_id[i], i);
	KT_EQ(acpi.nioapics, 1);
	KT_EQ(acpi.ioapic_addr[0], 0xfec00000);
	KT_EQ(acpi.ioapic_gsi_base[0], 0);
	KT_EQ(acpi.lapic_addr, 0xfee00000);
	KT_EQ(acpi.hpet_addr, 0xfed00000);
	KT_EQ(acpi.nmcfg, 1);
	KT_EQ(acpi.mcfg_base[0] & ((1 << 20) - 1), 0);
	KT_EQ(acpi.madt_bad_entries, 0);
	uint16_t fl;
	KT_EQ(acpi_isa_gsi(0, &fl), 2); /* the PIT is wired to GSI 2 */
	KT_EQ(acpi_isa_gsi(4, &fl), 4); /* COM1 identity mapped */
	KT_EQ(acpi.has_8042, !cmdline_has("kt-desktop")); /* QEMU clears the flag with i8042=off */
	/* the parser may only read ACPI memory: RAM and MMIO are refused */
	KT_ASSERT(magia_acpi_map(bootinfo.rsdp_pa, 20) != NULL);
	KT_ASSERT(magia_acpi_map(kernel_phys_base, 36) == NULL);
	KT_ASSERT(magia_acpi_map(0xfec00000, 36) == NULL);
	KT_ASSERT(magia_acpi_map(~0ULL - 8, 36) == NULL);
}

/* Console output does not reveal the KASLR slide or pass raw firmware
 * bytes through (security review stage 1, F2). */
KTEST(021, console_hides_slide_and_firmware_bytes)
{
	uint64_t off = 1;
	const char *name = ksym_lookup((uint64_t)ktest_run_all, &off);
	KT_ASSERT(name != NULL);
	uint64_t link = kaddr_unslid((uint64_t)ktest_run_all);
	KT_EQ(link, (uint64_t)ktest_run_all - bootinfo.kernel_slide);
	KT_ASSERT(link >= KERNEL_VMA && link < KERNEL_VMA + (64ULL << 20)); /* where nm puts it */
	KT_EQ(kaddr_unslid(0x1234), 0x1234); /* non-image values unchanged */
	char out[5];
	acpi_sig_printable("AP\x1b" "C", out);
	KT_EQ(strcmp(out, "AP?C"), 0);
	acpi_sig_printable("\x7f\x80 ~", out);
	KT_EQ(strcmp(out, "?? ~"), 0);
	acpi_sig_printable("FACP", out);
	KT_EQ(strcmp(out, "FACP"), 0);
}

/* ACPI power on q35: the FADT reset register (I/O 0xcf9, value 0x0f,
 * RESET_REG_SUP set), PM1a control at I/O 0x604, and \_S5 = {0, 0} read
 * from the DSDT's AML by rust/src/aml.rs. The paths themselves run in
 * tests/run-ktest.sh (the monitor run reboots, the USB runs power off). */
KTEST(022, acpi_power_fields_q35)
{
	KT_EQ(acpi.reset_ok, 1);
	KT_EQ(acpi.reset_space, ACPI_GAS_IO);
	KT_EQ(acpi.reset_addr, 0xcf9);
	KT_EQ(acpi.reset_value, 0x0f);
	KT_EQ(acpi.pm1a_space, ACPI_GAS_IO);
	KT_EQ(acpi.pm1a_cnt, 0x604);
	KT_EQ(acpi.pm1b_cnt, 0);
	KT_EQ(acpi.s5_found, 1);
	KT_EQ(acpi.s5_error, 0);
	KT_EQ(acpi.slp_typa, 0);
	KT_EQ(acpi.slp_typb, 0);
	KT_EQ(acpi.hw_reduced, 0);
	/* OVMF hands over in ACPI mode (SCI_EN set); SeaBIOS leaves legacy
	 * mode, and poweroff must be able to switch through SMI_CMD */
	if (bootinfo.firmware != BOOT_FW_BIOS)
		KT_EQ(inw(0x604) & 1, 1);
	KT_ASSERT(acpi.smi_cmd != 0 && acpi.acpi_enable != 0);
}
