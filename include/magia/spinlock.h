/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* spinlock.h - IRQ-saving spinlocks. Magia is uniprocessor today, so the
 * lock mostly documents intent and catches recursion; the atomic xchg makes
 * it correct once SMP arrives. */
#ifndef MAGIA_SPINLOCK_H
#define MAGIA_SPINLOCK_H

#include <magia/kernel.h>

typedef struct spinlock {
	volatile uint32_t locked;
	const char *name;
	uint64_t saved_flags;
} spinlock_t;

#define SPINLOCK_INIT(n) { 0, n, 0 }

void spin_lock(spinlock_t *l);   /* disables interrupts */
void spin_unlock(spinlock_t *l); /* restores them */
bool spin_held(const spinlock_t *l);

#endif
