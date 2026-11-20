/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * t_ring3.c - ring-3 execution and the debug monitor (9xx).
 *
 * A child process copies a blob from arch/x86_64/user_blob.S into a user
 * code window and drops to ring 3 with enter_user(). The blob makes real
 * `syscall` instructions; results land in a user data page that the parent
 * reads back after waitpid(). (Magia has one address space, so the parent
 * can see the child's window; per-process page tables are on the roadmap.)
 */
#include <magia/cpu.h>
#include <magia/ktest.h>
#include <magia/mm.h>
#include <magia/syscall.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

extern const char ublob_syscalls[], ublob_syscalls_end[];
extern const char ublob_hlt[], ublob_hlt_end[], ublob_kread[], ublob_kread_end[];
extern const char ublob_inb[], ublob_inb_end[], ublob_sgdt[], ublob_sgdt_end[];
extern volatile uint64_t user_faults;
int monitor_exec(char *line);

struct ring3_ctx {
	uint64_t rip, rsp, data;
};

static void ring3_child(void *arg)
{
	struct ring3_ctx *c = arg;
	enter_user(c->rip, c->rsp, c->data, 0, 0);
}

/* Run a blob in a child process; returns its exit status. */
static int run_blob(const char *start, const char *end, uint64_t data, int *pid_out)
{
	uint64_t code = 0, kva = 0, stack = 0;
	KT_EQ(uvm_alloc(0, 1, UVM_EXEC | UVM_CONTIG, &code, &kva), 0);
	KT_EQ(uvm_alloc(0, 2, 0, &stack, NULL), 0);
	memcpy((void *)kva, start, (size_t)(end - start));
	struct ring3_ctx c = { code, stack + 2 * PAGE_SIZE - 64, data };
	int pid = kt_spawn("ring3", ring3_child, &c);
	KT_ASSERT(pid > 0);
	if (pid_out)
		*pid_out = pid;
	int status = pid > 0 ? kt_wait(pid) : -1;
	KT_EQ(uvm_free(0, code, 1), 0);
	KT_EQ(uvm_free(0, stack, 2), 0);
	return status;
}

KTEST(900, ring3_syscall_sysret_roundtrip)
{
	uint64_t data = 0;
	KT_EQ(uvm_alloc(0, 1, 0, &data, NULL), 0);
	uint64_t sc0 = syscall_count;
	int pid = 0;
	int status = run_blob(ublob_syscalls, ublob_syscalls_end, data, &pid);
	KT_EQ(status, 42);
	int64_t r[8];
	kt_uget(r, data, sizeof(r));
	KT_EQ(r[0], pid);                                       /* getpid */
	KT_EQ(r[1], 40);                                        /* write returned the length */
	KT_EQ(r[2], -ENOSYS);                                   /* unknown syscall number */
	KT_EQ(r[3], -EFAULT);                                   /* kernel pointer rejected */
	KT_EQ(r[4], 1);                                         /* rbx, rbp, r12-r14 preserved */
	KT_ASSERT(r[5] & RFLAGS_IF);                            /* interrupts on in user mode */
	KT_EQ(r[5] & RFLAGS_AC, 0);
	KT_EQ(r[6], ksys0(SYS_getpid));                         /* parent is this test process */
	KT_EQ(r[7], 0x656e6f64);                                /* finished the long loop */
	KT_ASSERT(syscall_count - sc0 >= 7);
	KT_EQ(uvm_free(0, data, 1), 0);
}

KTEST(901, ring3_privileged_instruction_kills_process)
{
	uint64_t f0 = user_faults;
	KT_EQ(run_blob(ublob_hlt, ublob_hlt_end, 0, NULL), 128 + 13);
	KT_EQ(run_blob(ublob_inb, ublob_inb_end, 0, NULL), 128 + 13);
	KT_EQ(user_faults - f0, 2);
}

KTEST(902, ring3_kernel_memory_read_kills_process)
{
	KT_EQ(run_blob(ublob_kread, ublob_kread_end, 0, NULL), 128 + 14);
}

KTEST(903, ring3_umip_blocks_sgdt)
{
	int st = run_blob(ublob_sgdt, ublob_sgdt_end, 0, NULL);
	KT_EQ(st, umip_enabled ? 128 + 13 : 7);
}

KTEST(910, monitor_commands)
{
	static const char *const ok[] = { "help", "uname", "cpu", "ps", "threads", "jails", "mem",
					  "uptime", "ls /", "ls /dev", "cat /etc/motd",
					  "sysctl kern.ostype", "echo monitor ok" };
	char line[64];
	for (size_t i = 0; i < ARRAY_SIZE(ok); i++) {
		strlcpy(line, ok[i], sizeof(line));
		KT_EQ(monitor_exec(line), 0);
	}
	strlcpy(line, "no-such-command arg", sizeof(line));
	KT_EQ(monitor_exec(line), -1);
	strlcpy(line, "   ", sizeof(line));
	KT_EQ(monitor_exec(line), 0); /* empty line is fine */
}
