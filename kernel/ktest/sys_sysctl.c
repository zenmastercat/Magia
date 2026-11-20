/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* sys_sysctl.c - sysctl MIB, uname and the syscall table's validation (8xx). */
#include "kt_sys.h"

/* Read a node into buf; returns the result and stores the reported length. */
static int64_t sysctl_raw(const char *name, void *buf, uint64_t buflen, uint64_t *len_out)
{
	uint64_t n = kt_upath(name), old = buf ? kt_page() : 0, lenp = kt_page();
	kt_uput(lenp, &buflen, sizeof(buflen));
	int64_t r = ksys6(SYS_sysctl, n, strlen(name), old, lenp, 0, 0);
	*len_out = kt_get_u64(lenp);
	if (r == 0 && buf)
		kt_uget(buf, old, (size_t)*len_out);
	return r;
}

KTEST(800, sysctl_reads_strings_and_numbers)
{
	char s[64] = { 0 };
	uint64_t len = 0, v = 0;
	KT_EQ(sysctl_raw("kern.ostype", s, sizeof(s), &len), 0);
	KT_EQ(len, 6);
	KT_EQ(strcmp(s, "Magia"), 0);
	KT_EQ(kt_sysctl_u64("hw.pagesize", &v), 0);
	KT_EQ(v, 4096);
	KT_EQ(kt_sysctl_u64("hw.ncpu", &v), 0);
	KT_EQ(v, 1);
	KT_EQ(kt_sysctl_u64("hw.physmem", &v), 0);
	KT_EQ(v, pmm_total_pages() * PAGE_SIZE);
	KT_EQ(kt_sysctl_u64("security.jail.jailed", &v), 0);
	KT_EQ(v, 0);
	/* C nodes registered through the set_sysctl linker set */
	uint64_t t0 = ticks;
	KT_EQ(kt_sysctl_u64("kern.ticks", &v), 0);
	KT_ASSERT(v >= t0 && v <= ticks);
	KT_EQ(sysctl_raw("kern.build", NULL, 0, &len), 0); /* size query */
	KT_ASSERT(len > 1);
	/* buffer too small: ENOMEM and the needed size */
	KT_EQ(sysctl_raw("kern.ostype", s, 3, &len), KT_NEG(ENOMEM));
	KT_EQ(len, 6);
}

KTEST(801, sysctl_errors)
{
	uint64_t v = 0, len = 0;
	KT_EQ(kt_sysctl_u64("kern.no_such_node", &v), KT_NEG(ENOENT));
	KT_EQ(sysctl_raw("kern", NULL, 8, &len), KT_NEG(EISDIR));
	KT_EQ(kt_sysctl_set_u64("hw.ncpu", 4), KT_NEG(EPERM));
	KT_EQ(kt_sysctl_set_u64("kern.ticks", 0), KT_NEG(EPERM));
	KT_EQ(kt_sysctl_set_u64("kern.sched.base_slice_ns", 10), KT_NEG(EINVAL));
	KT_EQ(kt_sysctl_set_u64("debug.syscall_trace", 2), KT_NEG(EINVAL));
	uint64_t n = kt_upath("debug.syscall_trace"), newp = kt_page();
	KT_EQ(ksys6(SYS_sysctl, n, 19, 0, 0, newp, 4), KT_NEG(EINVAL)); /* wrong size */
	KT_EQ(ksys6(SYS_sysctl, n, 0, 0, 0, 0, 0), KT_NEG(EINVAL));      /* empty name */
	KT_EQ(ksys6(SYS_sysctl, n, 19, kt_page(), 0, 0, 0), KT_NEG(EINVAL)); /* oldp w/o oldlenp */
}

KTEST(802, sysctl_write_reaches_scheduler)
{
	uint64_t was = 0, v = 0;
	KT_EQ(kt_sysctl_u64("kern.sched.base_slice_ns", &was), 0);
	KT_EQ(was, 3000000);
	KT_EQ(kt_sysctl_set_u64("kern.sched.base_slice_ns", 2000000), 0);
	KT_EQ(kt_sysctl_u64("kern.sched.base_slice_ns", &v), 0);
	KT_EQ(v, 2000000);
	KT_EQ(kt_sysctl_set_u64("kern.sched.base_slice_ns", was), 0);
	/* a jailed process cannot change kernel-wide knobs */
	int64_t jid = kt_jail("/", "sysctl802", 0, 0, 0, 0);
	KT_ASSERT(jid > 0);
	uint64_t n = kt_upath("kern.sched.base_slice_ns"), newp = kt_page();
	kt_uput(newp, &was, 8);
	KT_EQ(ksys1(SYS_jail_attach, jid), 0);
	KT_EQ(ksys6(SYS_sysctl, n, 24, 0, 0, newp, 8), KT_NEG(EPERM));
}

KTEST(803, sysctl_in_capability_mode)
{
	char s[16];
	uint64_t len = 0, v = 0;
	uint64_t n = kt_upath("debug.syscall_trace"), newp = kt_page();
	KT_EQ(ksys0(SYS_cap_enter), 0);
	KT_EQ(sysctl_raw("kern.ostype", s, sizeof(s), &len), 0);
	KT_EQ(kt_sysctl_u64("security.capability_mode", &v), 0);
	KT_EQ(v, 1);
	KT_EQ(kt_sysctl_u64("kern.proc.nprocs", &v), KT_NEG(ECAPMODE));
	KT_EQ(ksys6(SYS_sysctl, n, 19, 0, 0, newp, 8), KT_NEG(ECAPMODE));
}

KTEST(804, syscall_table_validation)
{
	uint64_t u = kt_page();
	KT_EQ(ksys0(0), KT_NEG(ENOSYS));
	KT_EQ(ksys0(15), KT_NEG(ENOSYS));
	KT_EQ(ksys0(SYS_MAX), KT_NEG(ENOSYS));
	KT_EQ(ksys0(~0ULL), KT_NEG(ENOSYS));
	KT_EQ(ksys3(SYS_read, 64, u, 1), KT_NEG(EBADF));
	KT_EQ(ksys3(SYS_read, (uint64_t)-1, u, 1), KT_NEG(EBADF));
	KT_EQ(ksys3(SYS_read, 0, u, (uint64_t)-1), KT_NEG(EINVAL));
	KT_EQ(ksys3(SYS_lseek, 0, 0, 3), KT_NEG(EINVAL));
	KT_EQ(ksys2(SYS_setpriority, 0, 20), KT_NEG(EINVAL));
	KT_EQ(ksys3(SYS_waitpid, (uint64_t)-1, 0, 2), KT_NEG(EINVAL));
	/* order: EINVAL before EBADF before EFAULT */
	KT_EQ(ksys3(SYS_read, 99, 0, (uint64_t)-5), KT_NEG(EINVAL));
	KT_EQ(ksys3(SYS_read, 99, 0, 5), KT_NEG(EBADF));
	KT_EQ(ksys3(SYS_read, 0, 0, 5), KT_NEG(EFAULT));
	/* the trace knob prints calls on the console */
	KT_EQ(kt_sysctl_set_u64("debug.syscall_trace", 1), 0);
	KT_EQ(ksys1(SYS_close, 50), KT_NEG(EBADF));
	KT_EQ(kt_sysctl_set_u64("debug.syscall_trace", 0), 0);
}

KTEST(805, uname_reports_magia)
{
	struct utsname un;
	uint64_t u = kt_page();
	KT_EQ(ksys1(SYS_uname, u), 0);
	kt_uget(&un, u, sizeof(un));
	KT_EQ(strcmp(un.sysname, "Magia"), 0);
	KT_EQ(strcmp(un.release, MAGIA_RELEASE), 0);
	KT_EQ(strcmp(un.machine, "x86_64"), 0);
	KT_EQ(strcmp(un.nodename, "magia"), 0);
	KT_EQ(strncmp(un.version, "Magia 0.2.0 (experimental", 25), 0);
}
