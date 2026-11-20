/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * sysctl.h - C-side sysctl nodes registered through a linker set.
 *
 * Idea from FreeBSD's SYSCTL_* macros, which drop their oid into the
 * set_sysctl_set linker set: a C file can export a kernel counter without
 * editing any central table. The MIB tree itself lives in Rust
 * (rust/src/sysctl.rs); at rs_init() it walks this set through
 * magia_sysctl_c_set() and grafts every node into the tree.
 *
 *   SYSCTL_U64_RO(kern_ticks, "kern.ticks", &ticks, "timer ticks since boot");
 */
#ifndef MAGIA_SYSCTL_H
#define MAGIA_SYSCTL_H

#include <magia/linker_set.h>
#include <stddef.h>
#include <stdint.h>

#define CSYSCTL_U64_RO 1 /* ptr -> volatile uint64_t, read-only           */
#define CSYSCTL_STR_RO 2 /* ptr -> NUL-terminated string, read-only        */
#define CSYSCTL_U64_RW 3 /* ptr -> uint64_t, writable from the root jail   */

#define CSYSCTL_F_CAPRD 0x1 /* readable in Capsicum capability mode */

/* Mirrored in rust/src/sysctl.rs (CSysctl). */
struct c_sysctl {
	const char *name;  /* full dotted name, e.g. "kern.ticks" */
	uint32_t kind;     /* CSYSCTL_* */
	uint32_t flags;    /* CSYSCTL_F_* */
	void *ptr;
	const char *descr;
	uint64_t min, max; /* inclusive range for CSYSCTL_U64_RW */
};

MAGIA_SET_DECLARE(sysctl, const struct c_sysctl);

#define SYSCTL_C(uniq, nm, kd, fl, p, ds, mn, mx)                            \
	static const struct c_sysctl __sysctl_##uniq = { nm, kd, fl, (void *)(p), ds, mn, mx }; \
	MAGIA_SET(sysctl, 0, __sysctl_##uniq)

#define SYSCTL_U64_RO(uniq, nm, p, ds) SYSCTL_C(uniq, nm, CSYSCTL_U64_RO, CSYSCTL_F_CAPRD, p, ds, 0, 0)
#define SYSCTL_STR_RO(uniq, nm, p, ds) SYSCTL_C(uniq, nm, CSYSCTL_STR_RO, CSYSCTL_F_CAPRD, p, ds, 0, 0)
#define SYSCTL_U64_RW(uniq, nm, p, ds, mn, mx) SYSCTL_C(uniq, nm, CSYSCTL_U64_RW, 0, p, ds, mn, mx)

#endif
