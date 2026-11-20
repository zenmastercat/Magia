/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * trap.c - IDT setup and the C half of exception/IRQ handling.
 *
 * Kernel-mode exceptions panic with a register dump and backtrace, except:
 *   - #BP (int3) is counted and returns (ktest proves exceptions resume);
 *   - a #PF inside uaccess_copy is redirected to its -EFAULT fixup;
 *   - a fault while the thread armed fault_jmp resumes at ksetjmp.
 * User-mode exceptions kill the offending process (exit status 128+vector),
 * never the kernel.
 */
#include <magia/abi.h>
#include <magia/apic.h>
#include <magia/cpu.h>
#include <magia/rust.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

struct __packed idt_gate {
	uint16_t off_lo;
	uint16_t sel;
	uint8_t ist;
	uint8_t type;
	uint16_t off_mid;
	uint32_t off_hi;
	uint32_t _r;
};

static struct idt_gate idt[256] __aligned(16);
static irq_handler_t irq_handlers[16];
volatile uint64_t irq_counts[16];
volatile uint64_t vec_counts[2]; /* LAPIC timer, LAPIC error */
volatile uint64_t breakpoint_hits;
volatile uint64_t user_faults;
extern const uint64_t isr_table[];
extern char uaccess_copy[], uaccess_copy_end[], uaccess_fixup[];

static const char *const exc_names[32] = {
	"#DE divide error", "#DB debug", "NMI", "#BP breakpoint", "#OF overflow",
	"#BR bound range", "#UD invalid opcode", "#NM device not available",
	"#DF double fault", "coprocessor overrun", "#TS invalid TSS",
	"#NP segment not present", "#SS stack fault", "#GP general protection",
	"#PF page fault", "reserved", "#MF x87 error", "#AC alignment check",
	"#MC machine check", "#XM SIMD error", "#VE virtualization", "#CP control protection",
	"reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
	"#HV", "#VC", "#SX security", "reserved",
};

static void set_gate(int vec, uint64_t handler, uint8_t ist)
{
	idt[vec] = (struct idt_gate){
		.off_lo = handler & 0xffff,
		.sel = GDT_KCODE,
		.ist = ist,
		.type = 0x8e, /* present, DPL 0, 64-bit interrupt gate */
		.off_mid = (handler >> 16) & 0xffff,
		.off_hi = handler >> 32,
	};
}

void idt_init(void)
{
	for (int i = 0; i < 48; i++)
		set_gate(i, isr_table[i], i == 8 ? 1 : i == 2 ? 2 : 0);
	set_gate(VEC_LAPIC_TIMER, isr_table[48], 0);
	set_gate(VEC_LAPIC_ERROR, isr_table[49], 0);
	set_gate(VEC_SPURIOUS, isr_table[50], 0);
	struct __packed {
		uint16_t limit;
		uint64_t base;
	} idtr = { sizeof(idt) - 1, (uint64_t)idt };
	__asm__ volatile("lidt %0" ::"m"(idtr));
}

void irq_register(int irq, irq_handler_t h) { irq_handlers[irq] = h; }

/* Handlers for LAPIC vectors outside the ISA range (timer, error). */
static irq_handler_t vec_handlers[256];
void irq_register_vector(int vec, irq_handler_t h) { vec_handlers[vec] = h; }

void irq_enable(int irq)
{
	if (apic_mode)
		ioapic_route_isa(irq);
	else
		pic_unmask(irq);
}

void trap_dump(const struct trapframe *tf)
{
	const char *name = tf->vector < 32 ? exc_names[tf->vector] : "irq";
	kprintf("trap %lu (%s) err=0x%lx\n", tf->vector, name, tf->error);
	kprintf(" rip=%p cs=%lx rflags=%lx rsp=%p ss=%lx\n", (void *)tf->rip, tf->cs, tf->rflags,
		(void *)tf->rsp, tf->ss);
	kprintf(" rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx\n", tf->rax, tf->rbx, tf->rcx, tf->rdx);
	kprintf(" rsi=%016lx rdi=%016lx rbp=%016lx r8 =%016lx\n", tf->rsi, tf->rdi, tf->rbp, tf->r8);
	kprintf(" r9 =%016lx r10=%016lx r11=%016lx r12=%016lx\n", tf->r9, tf->r10, tf->r11, tf->r12);
	kprintf(" r13=%016lx r14=%016lx r15=%016lx cr2=%016lx\n", tf->r13, tf->r14, tf->r15, read_cr2());
	if (tf->vector == 14)
		kprintf(" #PF: %s %s %s%s\n", tf->error & 1 ? "protection" : "not-present",
			tf->error & 2 ? "write" : "read", tf->error & 4 ? "user" : "kernel",
			tf->error & 16 ? " instruction-fetch" : "");
}

static void user_fault(struct trapframe *tf)
{
	struct thread *t = curthread();
	user_faults++;
	kprintf("magia: pid %d (tid %d, %s) killed: %s at rip %p cr2 %p\n", t->pid, t->tid, t->name,
		exc_names[tf->vector], (void *)tf->rip, (void *)(tf->vector == 14 ? read_cr2() : 0));
	sti();
	uint64_t args[6] = { 128 + tf->vector, 0, 0, 0, 0, 0 };
	if (t->pid > 0)
		rs_syscall(t->pid, SYS_exit, args);
	thread_exit(128 + (int)tf->vector);
}

void trap_dispatch(struct trapframe *tf)
{
	uint64_t v = tf->vector;

	if (v >= IRQ_BASE && v < IRQ_BASE + 16) {
		int irq = (int)(v - IRQ_BASE);
		if (!apic_mode && (irq == 7 || irq == 15) && pic_spurious(irq))
			return;
		irq_counts[irq]++;
		if (irq_handlers[irq])
			irq_handlers[irq](tf);
		if (apic_mode)
			lapic_eoi();
		else
			pic_eoi(irq);
		preempt_check();
		return;
	}
	if (v == VEC_LAPIC_TIMER || v == VEC_LAPIC_ERROR) {
		vec_counts[v == VEC_LAPIC_TIMER ? 0 : 1]++;
		if (vec_handlers[v])
			vec_handlers[v](tf);
		lapic_eoi();
		preempt_check();
		return;
	}
	if (v == VEC_SPURIOUS)
		return; /* spurious LAPIC interrupt: no EOI */

	if (tf->cs & 3) {
		user_fault(tf);
		return;
	}

	if (v == 3) {
		breakpoint_hits++;
		return;
	}
	if (v == 14 && tf->rip >= (uint64_t)uaccess_copy && tf->rip < (uint64_t)uaccess_copy_end) {
		tf->rip = (uint64_t)uaccess_fixup;
		return;
	}
	struct thread *t = curthread();
	if (t && t->fault_jmp) {
		t->fault_vector = (int)v;
		t->fault_addr = v == 14 ? read_cr2() : 0;
		tf->rdi = (uint64_t)t->fault_jmp;
		tf->rsi = v + 1;
		tf->rip = (uint64_t)klongjmp;
		tf->rflags &= ~RFLAGS_AC;
		t->fault_jmp = NULL;
		return;
	}
	trap_dump(tf);
	backtrace_from(tf->rbp, tf->rip);
	panic("unhandled %s in kernel mode", v < 32 ? exc_names[v] : "trap");
}
