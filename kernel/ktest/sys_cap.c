/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* sys_cap.c - Capsicum capability mode and rights, through real syscalls (3xx). */
#include "kt_sys.h"

KTEST(300, cap_enter_blocks_global_namespaces)
{
	uint64_t m = kt_page();
	uint64_t path = kt_upath("/etc/motd");
	uint64_t jp = kt_page();
	KT_EQ(ksys1(SYS_cap_getmode, m), 0);
	KT_EQ(kt_get_u32(m), 0);
	KT_EQ(ksys0(SYS_cap_enter), 0);
	KT_EQ(ksys1(SYS_cap_getmode, m), 0);
	KT_EQ(kt_get_u32(m), 1);
	/* negative: global namespaces are gone */
	KT_EQ(ksys3(SYS_openat, KT_AT_FDCWD, path, O_RDONLY), KT_NEG(ECAPMODE));
	KT_EQ(ksys2(SYS_pslist, 0, 0), KT_NEG(ECAPMODE));
	KT_EQ(ksys1(SYS_jail_create, jp), KT_NEG(ECAPMODE));
	KT_EQ(ksys1(SYS_jail_attach, 0), KT_NEG(ECAPMODE));
	KT_EQ(ksys2(SYS_sethostname, path, 3), KT_NEG(ECAPMODE));
	/* positive: local operations still work */
	KT_ASSERT(ksys0(SYS_getpid) > 0);
	KT_ASSERT(ksys0(SYS_kqueue) >= 3);
	KT_EQ(kt_write_str(1, ""), 0);
}

KTEST(301, cap_rights_only_shrink)
{
	int64_t fd = kt_open(AT_FDCWD, "/etc/motd", O_RDONLY);
	uint64_t r = kt_page();
	char buf[8];
	KT_ASSERT(fd >= 3);
	KT_EQ(ksys2(SYS_cap_rights_limit, fd, CAP_READ | CAP_SEEK), 0);
	KT_EQ(ksys2(SYS_cap_rights_get, fd, r), 0);
	KT_EQ(kt_get_u64(r), CAP_READ | CAP_SEEK);
	/* negative: a right can never come back; unknown bits are invalid */
	KT_EQ(ksys2(SYS_cap_rights_limit, fd, CAP_READ | CAP_FSTAT), KT_NEG(ENOTCAPABLE));
	KT_EQ(ksys2(SYS_cap_rights_limit, fd, CAP_ALL), KT_NEG(ENOTCAPABLE));
	KT_EQ(ksys2(SYS_cap_rights_limit, fd, 1ULL << 40), KT_NEG(EINVAL));
	/* each operation checks its own right */
	KT_EQ(kt_read(fd, buf, 7), 7);
	KT_EQ(ksys3(SYS_lseek, fd, 0, SEEK_SET), 0);
	KT_EQ(ksys2(SYS_fstat, fd, kt_page()), KT_NEG(ENOTCAPABLE));
	KT_EQ(kt_write_str(fd, "x"), KT_NEG(ENOTCAPABLE));
	KT_EQ(ksys2(SYS_cap_rights_limit, fd, CAP_SEEK), 0);
	KT_EQ(kt_read(fd, buf, 1), KT_NEG(ENOTCAPABLE));
	/* dup keeps the reduced rights */
	int64_t d = ksys1(SYS_dup, fd);
	KT_ASSERT(d > fd);
	KT_EQ(ksys2(SYS_cap_rights_get, d, r), 0);
	KT_EQ(kt_get_u64(r), CAP_SEEK);
}

KTEST(302, cap_dirfd_lookups_stay_beneath)
{
	KT_EQ(ksys2(SYS_mkdirat, KT_AT_FDCWD, kt_upath("/tmp/cap302")), 0);
	int64_t d = kt_open(AT_FDCWD, "/tmp/cap302", O_RDONLY | O_DIRECTORY);
	KT_ASSERT(d >= 3);
	uint64_t up = kt_upath("../../etc/motd");
	uint64_t abs = kt_upath("/etc/motd");
	uint64_t ok = kt_upath("sub/../inside");
	KT_EQ(ksys0(SYS_cap_enter), 0);
	KT_EQ(ksys3(SYS_openat, d, up, O_RDONLY), KT_NEG(ENOTCAPABLE));
	KT_EQ(ksys3(SYS_openat, d, abs, O_RDONLY), KT_NEG(ENOTCAPABLE));
	int64_t f = ksys3(SYS_openat, d, kt_upath("inside"), O_CREAT | O_RDWR);
	KT_ASSERT(f >= 0);
	KT_EQ(kt_write_str(f, "sandboxed"), 9);
	KT_EQ(ksys2(SYS_mkdirat, d, kt_upath("sub")), 0);
	KT_ASSERT(ksys3(SYS_openat, d, ok, O_RDONLY) >= 0); /* ".." inside is fine */
	/* the dirfd's rights bound what can be opened through it */
	KT_EQ(ksys2(SYS_cap_rights_limit, d, CAP_LOOKUP | CAP_READ), 0);
	KT_EQ(ksys3(SYS_openat, d, kt_upath("inside"), O_RDWR), KT_NEG(ENOTCAPABLE));
	KT_EQ(ksys2(SYS_mkdirat, d, kt_upath("sub2")), KT_NEG(ENOTCAPABLE));
	KT_ASSERT(ksys3(SYS_openat, d, kt_upath("inside"), O_RDONLY) >= 0);
}

static void cap_child(void *arg)
{
	uint64_t path = (uint64_t)arg;
	int64_t r = ksys3(SYS_openat, KT_AT_FDCWD, path, O_RDONLY);
	ksys1(SYS_exit, r == KT_NEG(ECAPMODE) ? 0 : 1);
}

KTEST(303, cap_mode_is_inherited_by_children)
{
	uint64_t path = kt_upath("/etc/motd");
	KT_EQ(ksys0(SYS_cap_enter), 0);
	int pid = kt_spawn("capchild", cap_child, (void *)path);
	KT_ASSERT(pid > 0);
	KT_EQ(kt_wait(pid), 0);
}

KTEST(304, syscalls_reject_kernel_and_bad_pointers)
{
	char kbuf[16] = "kernel secret";
	uint64_t u = kt_page();
	/* kernel addresses are never valid user pointers */
	KT_EQ(ksys3(SYS_write, 1, (uint64_t)kbuf, 4), KT_NEG(EFAULT));
	KT_EQ(ksys3(SYS_read, 0, (uint64_t)&ktest_checks, 4), KT_NEG(EFAULT));
	KT_EQ(ksys2(SYS_fstat, 0, KERNEL_VMA), KT_NEG(EFAULT));
	KT_EQ(ksys1(SYS_uname, PHYS_MAP_BASE), KT_NEG(EFAULT));
	/* null, unmapped, straddling USER_TOP, wrapping */
	KT_EQ(ksys3(SYS_write, 1, 0, 4), KT_NEG(EFAULT));
	KT_EQ(ksys3(SYS_write, 1, 0x1000, 4), KT_NEG(EFAULT));
	KT_EQ(ksys3(SYS_write, 1, USER_TOP - 2, 4), KT_NEG(EFAULT));
	KT_EQ(ksys2(SYS_fstat, 0, ~0ULL - 8), KT_NEG(EFAULT));
	/* a buffer running off the end of a mapping */
	KT_EQ(ksys3(SYS_write, 1, u + PAGE_SIZE - 2, 4), KT_NEG(EFAULT));
	/* the positive case still works */
	KT_EQ(ksys1(SYS_uname, u), 0);
}
