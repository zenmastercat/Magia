/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* spinlock.c - IRQ-saving spinlocks (see include/magia/spinlock.h). */
#include <magia/spinlock.h>
#include <magia/x86.h>

void spin_lock(spinlock_t *l)
{
	uint64_t f = irq_save();
	if (l->locked && !panicking) {
		/* Uniprocessor with IRQs off: a held lock can only mean
		 * recursion, which would deadlock on SMP. */
		panic("spin_lock: recursive acquire of \"%s\"", l->name);
	}
	while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
		cpu_relax();
	l->saved_flags = f;
}

void spin_unlock(spinlock_t *l)
{
	uint64_t f = l->saved_flags;
	__atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
	irq_restore(f);
}

bool spin_held(const spinlock_t *l) { return l->locked != 0; }
