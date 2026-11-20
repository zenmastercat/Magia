/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * syscall.h - the C side of system call entry.
 *
 * Ring 3 enters through the `syscall` instruction (arch/x86_64/syscall.S ->
 * syscall_c()). Kernel threads that act as processes (ktest) call
 * ksyscall() instead. Both paths end in the same Rust dispatcher,
 * rs_syscall(), so argument validation is identical.
 */
#ifndef MAGIA_SYSCALL_H
#define MAGIA_SYSCALL_H

#include <magia/abi.h>
#include <magia/kernel.h>

int64_t ksyscall(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

#define ksys0(n)                   ksyscall((n), 0, 0, 0, 0, 0, 0)
#define ksys1(n, a)                ksyscall((n), (uint64_t)(a), 0, 0, 0, 0, 0)
#define ksys2(n, a, b)             ksyscall((n), (uint64_t)(a), (uint64_t)(b), 0, 0, 0, 0)
#define ksys3(n, a, b, c)          ksyscall((n), (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), 0, 0, 0)
#define ksys4(n, a, b, c, d)       ksyscall((n), (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), 0, 0)
#define ksys5(n, a, b, c, d, e)    ksyscall((n), (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), (uint64_t)(e), 0)
#define ksys6(n, a, b, c, d, e, f) ksyscall((n), (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), (uint64_t)(e), (uint64_t)(f))

extern volatile uint64_t syscall_count;

#endif
