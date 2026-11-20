/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * linker_set.h - link-time registration ("linker sets").
 *
 * Idea from FreeBSD's SET_DECLARE/DATA_SET and SYSINIT: each registration
 * drops a *pointer* into a named ELF section; the linker script gathers
 * all pointers of a set into one array delimited by __start_set_<name> and
 * __stop_set_<name>, and the kernel walks that array at boot. No central
 * list has to be edited when a subsystem or test is added.
 *
 * The section name carries a sort key ("set_<name>.<key>") and the linker
 * script uses SORT(), so sets are ordered by key at link time. SYSINIT uses
 * the key for its boot order.
 */
#ifndef MAGIA_LINKER_SET_H
#define MAGIA_LINKER_SET_H

#define __MAGIA_CONCAT2(a, b) a##b
#define __MAGIA_CONCAT(a, b) __MAGIA_CONCAT2(a, b)
#define __MAGIA_STR2(x) #x
#define __MAGIA_STR(x) __MAGIA_STR2(x)

/* Declare the start/stop symbols of a set holding pointers to `type`. */
#define MAGIA_SET_DECLARE(set, type)                                         \
	extern type *const __start_set_##set[];                              \
	extern type *const __stop_set_##set[]

/* Add a pointer to `sym` to linker set `set`, sorted by `key`. */
#define MAGIA_SET(set, key, sym)                                             \
	static const void *const __MAGIA_CONCAT(__set_##set##_, sym)         \
		__attribute__((section("set_" #set "." __MAGIA_STR(key)), used, aligned(8))) = &(sym)

#define MAGIA_SET_FOREACH(pvar, set)                                         \
	for (pvar = __start_set_##set; pvar < __stop_set_##set; pvar++)

#define MAGIA_SET_COUNT(set) ((size_t)(__stop_set_##set - __start_set_##set))

/*
 * SYSINIT: boot-time init hooks, run in ascending `order` by kmain().
 * Orders are 5-digit numbers so that lexical sort == numeric sort.
 */
struct sysinit {
	const char *name;
	void (*func)(void);
};

#define SI_ORDER_EARLY   10000 /* console, cpu tables       */
#define SI_ORDER_VM      20000 /* page allocator, paging     */
#define SI_ORDER_KMEM    30000 /* zones / kmalloc            */
#define SI_ORDER_INTR    40000 /* PIC, timer                 */
#define SI_ORDER_SCHED   50000 /* threads, scheduler         */
#define SI_ORDER_RUST    60000 /* Rust subsystems            */
#define SI_ORDER_SYSCALL 70000 /* syscall MSRs               */
#define SI_ORDER_LAST    90000

#define SYSINIT(uniq, order, fn)                                             \
	static const struct sysinit __sysinit_##uniq = { #uniq, fn };        \
	MAGIA_SET(sysinit, order, __sysinit_##uniq)

MAGIA_SET_DECLARE(sysinit, const struct sysinit);

#endif
