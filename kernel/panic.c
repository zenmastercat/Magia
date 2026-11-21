/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * panic.c - panic, symbolized backtraces, stack protector, machine control.
 *
 * Backtraces walk the frame-pointer chain (everything is built with
 * -fno-omit-frame-pointer / force-frame-pointers) and look addresses up in
 * the sorted symbol table that the second link pass embeds in .ksyms.
 */
#include <magia/fbcon.h>
#include <magia/kernel.h>
#include <magia/x86.h>

volatile int panicking;
uint64_t __stack_chk_guard __section(".data") = 0x595e9fbd94fda766ULL;

struct ksym {
	uint64_t addr;     /* link-time address */
	uint64_t name_off; /* into ksym_strtab */
};
extern const uint64_t ksym_count;
extern const struct ksym ksym_table[];
extern const char ksym_strtab[];
extern char __text_start[], __text_end[];

/* The table holds link-time addresses (nm of the first link pass); the
 * running image is slid by KASLR, so look up addr - slide. */
const char *ksym_lookup(uint64_t addr, uint64_t *offset)
{
	uint64_t lo = 0, hi = ksym_count;
	if (hi == 0 || addr < (uint64_t)__text_start || addr >= (uint64_t)__text_end)
		return NULL;
	addr -= kernel_virt_base - KERNEL_VMA;
	if (addr < ksym_table[0].addr)
		return NULL;
	while (hi - lo > 1) {
		uint64_t mid = (lo + hi) / 2;
		if (ksym_table[mid].addr <= addr)
			lo = mid;
		else
			hi = mid;
	}
	if (offset)
		*offset = addr - ksym_table[lo].addr;
	return ksym_strtab + ksym_table[lo].name_off;
}

/* Kernel text addresses are printed at their link-time value (as in
 * `nm build/magia.elf`), so a console log does not reveal the KASLR slide. */
uint64_t kaddr_unslid(uint64_t a)
{
	extern char __kernel_start[], __kernel_end[];
	if (a >= (uint64_t)__kernel_start && a < (uint64_t)__kernel_end)
		return a - (kernel_virt_base - KERNEL_VMA);
	return a;
}

static void print_frame(uint64_t rip)
{
	uint64_t off = 0;
	const char *name = ksym_lookup(rip, &off);
	if (name)
		kprintf("  [<%p>] %s+0x%lx\n", (void *)kaddr_unslid(rip), name, off);
	else
		kprintf("  [<%p>] ?\n", (void *)kaddr_unslid(rip));
}

void backtrace_from(uint64_t rbp, uint64_t rip)
{
	kprintf("backtrace:\n");
	if (rip)
		print_frame(rip);
	for (int depth = 0; depth < 16; depth++) {
		if (rbp < dmap_base)
			break;
		if (rbp & 7)
			break;
		uint64_t *frame = (uint64_t *)rbp;
		uint64_t ret = frame[1];
		if (ret < (uint64_t)__text_start || ret >= (uint64_t)__text_end)
			break;
		print_frame(ret);
		rbp = frame[0];
	}
}

void panic(const char *fmt, ...)
{
	cli();
	if (panicking++) {
		/* recursive panic: just stop */
		for (;;)
			hlt();
	}
	static char msg[160];
	va_list ap;
	va_start(ap, fmt);
	kvsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	kprintf("\n*** Magia panic: %s ***\n", msg);
	backtrace_from(read_rbp(), 0);
	/* no serial port on the target machines: the screen is the report */
	fbcon_panic_screen(msg);
	if (ktest_exit_mode) {
		kprintf("KTEST PANIC\n");
		qemu_exit(3);
	}
	machine_halt();
}

__noreturn void __stack_chk_fail(void);
void __stack_chk_fail(void)
{
	panic("stack protector: stack smashing detected");
}

/* ---- machine control ---- */

void qemu_exit(uint32_t code)
{
	/* isa-debug-exit: QEMU exits with status (code << 1) | 1 */
	outl(0xf4, code);
}

void machine_halt(void)
{
	cli();
	for (;;)
		hlt();
}

void machine_reboot(void)
{
	cli();
	for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++)
		cpu_relax();
	outb(0x64, 0xfe); /* 8042 pulse reset line */
	/* triple fault as a fallback */
	struct __packed {
		uint16_t limit;
		uint64_t base;
	} idtr = { 0, 0 };
	__asm__ volatile("lidt %0; int3" ::"m"(idtr));
	machine_halt();
}
