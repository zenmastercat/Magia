/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* sys_jail.c - jails, visibility and hierarchical resource limits (4xx). */
#include "kt_sys.h"

KTEST(400, jail_create_get_attach_changes_root)
{
	KT_EQ(ksys2(SYS_mkdirat, KT_AT_FDCWD, kt_upath("/tmp/j400")), 0);
	int64_t f = kt_open(AT_FDCWD, "/tmp/j400/inside", O_CREAT | O_RDWR);
	KT_EQ(kt_write_str(f, "jailed"), 6);
	int64_t jid = kt_jail("/tmp/j400", "j400", 1 << 20, 8, 0, 0);
	KT_ASSERT(jid > 0);
	struct jail_info ji;
	uint64_t u = kt_page();
	KT_EQ(ksys2(SYS_jail_get, jid, u), 0);
	kt_uget(&ji, u, sizeof(ji));
	KT_EQ(ji.jid, jid);
	KT_EQ(ji.parent, 0);
	KT_EQ(ji.depth, 1);
	KT_EQ(ji.cpu_weight, 100);
	KT_EQ(ji.mem_max, 1 << 20);
	KT_EQ(ji.pids_max, 8);
	KT_EQ(strcmp(ji.hostname, "j400"), 0);
	KT_EQ(strcmp(ji.path, "/tmp/j400"), 0);
	/* pre-allocate user memory used after attaching */
	uint64_t p_inside = kt_upath("/inside"), p_motd = kt_upath("/../etc/motd");
	uint64_t host = kt_page(), un = kt_page(), buf = kt_page();
	KT_EQ(ksys1(SYS_jail_attach, jid), 0);
	int64_t g = ksys3(SYS_openat, KT_AT_FDCWD, p_inside, O_RDONLY);
	KT_ASSERT(g >= 0);
	KT_EQ(ksys3(SYS_read, g, buf, 64), 6);
	KT_EQ(ksys3(SYS_openat, KT_AT_FDCWD, p_motd, O_RDONLY), KT_NEG(ENOENT));
	KT_EQ(ksys2(SYS_gethostname, host, 64), 0);
	char hn[8] = { 0 };
	kt_uget(hn, host, 5);
	KT_EQ(strcmp(hn, "j400"), 0);
	struct utsname uts;
	KT_EQ(ksys1(SYS_uname, un), 0);
	kt_uget(&uts, un, sizeof(uts));
	KT_EQ(strcmp(uts.nodename, "j400"), 0);
	/* negative: no way out, no peeking up, no raising own limits */
	KT_EQ(ksys1(SYS_jail_attach, 0), KT_NEG(EPERM));
	KT_EQ(ksys2(SYS_jail_get, 0, u), KT_NEG(ESRCH));
	KT_EQ(ksys3(SYS_jail_set, jid, JAIL_SET_MEM_MAX, 1 << 30), KT_NEG(EPERM));
	KT_EQ(ksys1(SYS_jail_attach, 999), KT_NEG(ESRCH));
}

KTEST(401, jail_mem_limit_gives_enomem)
{
	int64_t jid = kt_jail("/", "mem401", 4 * PAGE_SIZE, 0, 0, 0);
	KT_ASSERT(jid > 0);
	KT_EQ(ksys1(SYS_jail_attach, jid), 0);
	int64_t a = ksys4(SYS_mmap, 0, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE, 0);
	KT_ASSERT(a > 0);
	KT_EQ(ksys4(SYS_mmap, 0, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE, 0), KT_NEG(ENOMEM));
	int64_t b = ksys4(SYS_mmap, 0, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE, 0);
	KT_ASSERT(b > 0);
	KT_EQ(ksys4(SYS_mmap, 0, PAGE_SIZE, PROT_READ, 0), KT_NEG(ENOMEM));
	KT_EQ(ksys2(SYS_munmap, a, 2 * PAGE_SIZE), 0);
	KT_ASSERT(ksys4(SYS_mmap, 0, PAGE_SIZE, PROT_READ, 0) > 0);
	/* other mmap rules */
	KT_EQ(ksys4(SYS_mmap, 0x10000000, PAGE_SIZE, PROT_READ, 0), KT_NEG(EINVAL));
	KT_EQ(ksys4(SYS_mmap, 0, PAGE_SIZE, PROT_READ | PROT_EXEC, 0), KT_NEG(EACCES));
	KT_EQ(ksys2(SYS_munmap, b + PAGE_SIZE, PAGE_SIZE), KT_NEG(EINVAL));
}

static void sleeper(void *arg)
{
	ksys1(SYS_nanosleep, (uint64_t)arg * 1000000ULL);
	ksys1(SYS_exit, 3);
}

KTEST(402, jail_pids_max_gives_eagain)
{
	int64_t jid = kt_jail("/", "pids402", 0, 2, 0, 0);
	KT_ASSERT(jid > 0);
	KT_EQ(ksys1(SYS_jail_attach, jid), 0); /* this process is #1 */
	int c1 = kt_spawn("c1", sleeper, (void *)50);
	KT_ASSERT(c1 > 0); /* #2 */
	int c2 = kt_spawn("c2", sleeper, (void *)1);
	KT_EQ(c2, -EAGAIN);
	KT_EQ(kt_wait(c1), 3);
	int c3 = kt_spawn("c3", sleeper, (void *)1);
	KT_ASSERT(c3 > 0); /* slot freed by reaping c1 */
	KT_EQ(kt_wait(c3), 3);
}

KTEST(403, jail_create_validation)
{
	KT_EQ(kt_jail("/nonexistent", "x", 0, 0, 0, 0), KT_NEG(ENOENT));
	KT_EQ(kt_jail("/etc/motd", "x", 0, 0, 0, 0), KT_NEG(ENOTDIR));
	KT_EQ(kt_jail("/", "x", 0, 0, 10001, 0), KT_NEG(EINVAL));
	KT_EQ(kt_jail("/", "x", 0, 0, 0, 0x80), KT_NEG(EINVAL));
	KT_EQ(kt_jail("/", "this-hostname-is-far-too-long-for-a-jail-because-it-has-64-chars", 0, 0, 0, 0),
	      KT_NEG(ENAMETOOLONG));
	KT_EQ(ksys1(SYS_jail_create, 0x5000), KT_NEG(EFAULT));
	/* a child jail cannot exceed its parent's limits or privileges */
	int64_t outer = kt_jail("/", "outer", 1 << 20, 0, 0, 0);
	KT_ASSERT(outer > 0);
	struct jail_params big = { kt_upath("/"), kt_upath("in"), 2 << 20, 0, 4, 0, 0 };
	struct jail_params priv = { kt_upath("/"), kt_upath("in"), 0, 0, 4, 0, JAIL_ALLOW_SET_HOSTNAME };
	uint64_t ub = kt_page(), up = kt_page();
	kt_uput(ub, &big, sizeof(big));
	kt_uput(up, &priv, sizeof(priv));
	KT_EQ(ksys1(SYS_jail_attach, outer), 0);
	KT_EQ(ksys1(SYS_jail_create, ub), KT_NEG(EINVAL));
	KT_EQ(ksys1(SYS_jail_create, up), KT_NEG(EPERM));
}

KTEST(404, jail_hides_outside_processes)
{
	int64_t jid = kt_jail("/tmp", "vis404", 0, 0, 0, 0);
	KT_ASSERT(jid > 0);
	int me = (int)ksys0(SYS_getpid);
	uint64_t pi = kt_page(), list = kt_page(), h = kt_upath("newname");
	KT_ASSERT(ksys2(SYS_pslist, list, 64) >= 2); /* sees pid 0 and itself */
	KT_EQ(ksys2(SYS_procinfo, 0, pi), 0);       /* pid 0 visible from jail 0 */
	KT_EQ(ksys1(SYS_jail_attach, jid), 0);
	KT_EQ(ksys2(SYS_pslist, list, 64), 1);
	int32_t only = 0;
	kt_uget(&only, list, sizeof(only));
	KT_EQ(only, me);
	KT_EQ(ksys2(SYS_procinfo, 1, pi), me == 1 ? 0 : KT_NEG(ESRCH));
	KT_EQ(ksys2(SYS_setpriority, 1, 5), me == 1 ? 0 : KT_NEG(ESRCH));
	struct proc_info info;
	KT_EQ(ksys2(SYS_procinfo, 0, pi), 0);
	kt_uget(&info, pi, sizeof(info));
	KT_EQ(info.pid, me);
	KT_EQ(info.jid, jid);
	/* no JAIL_ALLOW_SET_HOSTNAME */
	KT_EQ(ksys2(SYS_sethostname, h, 7), KT_NEG(EPERM));
	/* a jailed process may lower but not raise its priority */
	KT_EQ(ksys2(SYS_setpriority, 0, 5), 0);
	KT_EQ(ksys2(SYS_setpriority, 0, 0), KT_NEG(EPERM));
}

KTEST(405, jail_cpu_weight_scales_scheduler_weight)
{
	int64_t jid = kt_jail("/", "cpu405", 0, 0, 200, 0);
	KT_ASSERT(jid > 0);
	uint64_t pi = kt_page();
	struct proc_info info;
	KT_EQ(ksys1(SYS_jail_attach, jid), 0);
	KT_EQ(ksys2(SYS_procinfo, 0, pi), 0);
	kt_uget(&info, pi, sizeof(info));
	KT_EQ(info.weight, 2048);
	KT_EQ(ksys2(SYS_sched_setweight, 0, 512), 0);
	KT_EQ(ksys2(SYS_procinfo, 0, pi), 0);
	kt_uget(&info, pi, sizeof(info));
	KT_EQ(info.weight, 1024);
	KT_EQ(ksys2(SYS_sched_setweight, 0, 0), KT_NEG(EINVAL));
}
