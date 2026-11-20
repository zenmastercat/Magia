/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * main.c - kmain: banner, SYSINIT hooks in link-time order, then either
 * the in-kernel test run (cmdline "ktest") or the magia> debug monitor.
 */
#include <magia/cpu.h>
#include <magia/ktest.h>
#include <magia/linker_set.h>
#include <magia/mm.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

char kernel_cmdline[256];
bool ktest_exit_mode;
extern uint64_t boot_mbi_phys;
void monitor_main(void);
void kmain(uint32_t magic, uint32_t mbi_phys);

bool cmdline_has(const char *word)
{
	size_t n = strlen(word);
	for (const char *p = kernel_cmdline; *p;) {
		while (*p == ' ')
			p++;
		const char *e = p;
		while (*e && *e != ' ')
			e++;
		if ((size_t)(e - p) == n && strncmp(p, word, n) == 0)
			return true;
		p = e;
	}
	return false;
}

/*
 * Deliberate crashes, used by tests/run-ktest.sh as negative tests of the
 * hardening: each must end in a panic with a specific message.
 */
static __attribute__((noinline)) void crash_stack_protector(volatile int n)
{
	char buf[16];
	for (volatile int i = 0; i < n; i++)
		buf[i] = 'A'; /* overruns buf into the canary */
	__asm__ volatile("" ::"r"(buf) : "memory");
}

static void crash_thread_canary(void)
{
	*(volatile uint64_t *)curthread()->stack = 0; /* as if the stack overflowed */
	sched_yield();
	thread_sleep_ms(10); /* the next switch checks the canary */
}

static void crash_tests(void)
{
	if (cmdline_has("crash=stackprot"))
		crash_stack_protector(64);
	if (cmdline_has("crash=canary"))
		crash_thread_canary();
	if (cmdline_has("crash=wx")) {
		*(volatile uint8_t *)(uint64_t)kmain = 0xcc;
	}
}

static void interrupts_on(void) { sti(); }
SYSINIT(interrupts_on, 50001, interrupts_on); /* after sched_init (SI_ORDER_SCHED) */

void kmain(uint32_t magic, uint32_t mbi_phys)
{
	console_init();
	kprintf("\n" MAGIA_NAME " " MAGIA_RELEASE " (x86_64) - experimental hobby/research kernel\n");
	kprintf("Written from scratch in C and Rust; ideas from FreeBSD and Linux, no code from either.\n");
	if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
		panic("not booted by a multiboot loader (magic %x)", magic);
	boot_mbi_phys = mbi_phys;
	const uint32_t *mbi = phys_to_virt(mbi_phys);
	if (mbi[0] & (1 << 2))
		strlcpy(kernel_cmdline, phys_to_virt(mbi[4]), sizeof(kernel_cmdline));
	ktest_exit_mode = cmdline_has("ktest");
	kprintf("cmdline: \"%s\"\n", kernel_cmdline);

	const struct sysinit *const *si;
	MAGIA_SET_FOREACH(si, sysinit) {
		if (cmdline_has("verbose"))
			kprintf("sysinit: %s\n", (*si)->name);
		(*si)->func();
	}
	cpu_report();
	pmm_dump();
	kprintf("boot: %zu SYSINIT hooks, %zu ktests linked, %lu ms since timer start\n",
		MAGIA_SET_COUNT(sysinit), MAGIA_SET_COUNT(ktest), ticks);

	crash_tests();
	if (ktest_exit_mode) {
		int failed = ktest_run_all();
		qemu_exit(failed ? 1 : 0x10);
		machine_halt(); /* QEMU exits asynchronously */
	}
	monitor_main();
	machine_halt();
}
