/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * kt_sys.h - small helpers for the system-call ktests (sys_*.c).
 *
 * System calls only accept user addresses, so every buffer goes through a
 * fresh user page (kt_umem) and kt_uput/kt_uget. Pages are freed when the
 * test process exits.
 */
#ifndef MAGIA_KT_SYS_H
#define MAGIA_KT_SYS_H

#include <magia/ktest.h>
#include <magia/mm.h>
#include <magia/syscall.h>
#include <magia/trap.h>

#define KT_AT_FDCWD ((uint64_t)(int64_t)AT_FDCWD)
#define KT_NEG(e) (-(int64_t)(e))

static inline uint64_t kt_page(void) { return kt_umem(PAGE_SIZE); }

static inline uint64_t kt_upath(const char *s) { return kt_ustr(kt_page(), s); }

static inline int64_t kt_open(int64_t dirfd, const char *path, uint64_t flags)
{
	return ksys3(SYS_openat, (uint64_t)dirfd, kt_upath(path), flags);
}

static inline int64_t kt_write_str(int64_t fd, const char *s)
{
	size_t n = strlen(s);
	uint64_t u = kt_page();
	kt_uput(u, s, n);
	return ksys3(SYS_write, fd, u, n);
}

/* read up to n bytes into dst (NUL-terminated when n < cap) */
static inline int64_t kt_read(int64_t fd, char *dst, size_t n)
{
	uint64_t u = kt_page();
	int64_t r = ksys3(SYS_read, fd, u, n);
	if (r > 0)
		kt_uget(dst, u, (size_t)r);
	return r;
}

static inline int64_t kt_pipe(int64_t *rfd, int64_t *wfd)
{
	uint64_t u = kt_page();
	int64_t r = ksys1(SYS_pipe, u);
	int32_t fds[2] = { -1, -1 };
	if (r == 0)
		kt_uget(fds, u, sizeof(fds));
	*rfd = fds[0];
	*wfd = fds[1];
	return r;
}

static inline uint64_t kt_get_u64(uint64_t u)
{
	uint64_t v = 0;
	kt_uget(&v, u, sizeof(v));
	return v;
}

static inline uint32_t kt_get_u32(uint64_t u)
{
	uint32_t v = 0;
	kt_uget(&v, u, sizeof(v));
	return v;
}

/* jail_create with a user copy of the parameters; returns jid or -errno */
static inline int64_t kt_jail(const char *path, const char *host, uint64_t mem_max, uint32_t pids_max,
			      uint32_t cpu_weight, uint32_t flags)
{
	struct jail_params jp = {
		.path = kt_upath(path),
		.hostname = kt_upath(host),
		.mem_max = mem_max,
		.cpu_weight = cpu_weight,
		.children_max = 4,
		.pids_max = pids_max,
		.flags = flags,
	};
	uint64_t u = kt_page();
	kt_uput(u, &jp, sizeof(jp));
	return ksys1(SYS_jail_create, u);
}

/* sysctl read of a u64 node; returns 0 or -errno */
static inline int64_t kt_sysctl_u64(const char *name, uint64_t *out)
{
	uint64_t n = kt_upath(name), old = kt_page(), lenp = kt_page();
	uint64_t len = 8;
	kt_uput(lenp, &len, sizeof(len));
	int64_t r = ksys6(SYS_sysctl, n, strlen(name), old, lenp, 0, 0);
	if (r == 0)
		*out = kt_get_u64(old);
	return r;
}

static inline int64_t kt_sysctl_set_u64(const char *name, uint64_t v)
{
	uint64_t n = kt_upath(name), newp = kt_page();
	kt_uput(newp, &v, sizeof(v));
	return ksys6(SYS_sysctl, n, strlen(name), 0, 0, newp, sizeof(v));
}

#endif
