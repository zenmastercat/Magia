/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* sys_vfs.c - ramfs, devfs, pipes, fd table, processes (5xx). */
#include "kt_sys.h"

KTEST(500, vfs_create_write_seek_read)
{
	char buf[32] = { 0 };
	int64_t fd = kt_open(AT_FDCWD, "/tmp/f500", O_CREAT | O_RDWR);
	KT_EQ(fd, 3); /* 0-2 are the console */
	KT_EQ(kt_write_str(fd, "hello, world"), 12);
	KT_EQ(ksys3(SYS_lseek, fd, 7, SEEK_SET), 7);
	KT_EQ(kt_read(fd, buf, sizeof(buf) - 1), 5);
	KT_EQ(strcmp(buf, "world"), 0);
	KT_EQ(kt_read(fd, buf, 4), 0); /* EOF */
	KT_EQ(ksys3(SYS_lseek, fd, 0, SEEK_END), 12);
	KT_EQ(ksys3(SYS_lseek, fd, -100, SEEK_CUR), KT_NEG(EINVAL));
	struct m_stat st;
	uint64_t u = kt_page();
	KT_EQ(ksys2(SYS_fstat, fd, u), 0);
	kt_uget(&st, u, sizeof(st));
	KT_EQ(st.size, 12);
	KT_EQ(st.mode & S_IFMT, S_IFREG);
	KT_EQ(ksys1(SYS_close, fd), 0);
	KT_EQ(ksys1(SYS_close, fd), KT_NEG(EBADF));
	/* O_TRUNC + O_APPEND */
	fd = kt_open(AT_FDCWD, "/tmp/f500", O_WRONLY | O_TRUNC);
	KT_EQ(kt_write_str(fd, "ab"), 2);
	int64_t ap = kt_open(AT_FDCWD, "/tmp/f500", O_WRONLY | O_APPEND);
	KT_EQ(kt_write_str(ap, "cd"), 2);
	int64_t rd = kt_open(AT_FDCWD, "/tmp/f500", O_RDONLY);
	memset(buf, 0, sizeof(buf));
	KT_EQ(kt_read(rd, buf, 31), 4);
	KT_EQ(strcmp(buf, "abcd"), 0);
	KT_EQ(kt_write_str(rd, "x"), KT_NEG(EBADF)); /* opened read-only */
}

KTEST(501, vfs_open_errors)
{
	KT_EQ(kt_open(AT_FDCWD, "/tmp/missing", O_RDONLY), KT_NEG(ENOENT));
	KT_EQ(kt_open(AT_FDCWD, "/etc/motd", O_CREAT | O_EXCL | O_RDWR), KT_NEG(EEXIST));
	KT_EQ(kt_open(AT_FDCWD, "/tmp", O_RDWR), KT_NEG(EISDIR));
	KT_EQ(kt_open(AT_FDCWD, "/etc/motd", O_RDONLY | O_DIRECTORY), KT_NEG(ENOTDIR));
	KT_EQ(kt_open(AT_FDCWD, "/etc/motd/x", O_RDONLY), KT_NEG(ENOTDIR));
	KT_EQ(kt_open(AT_FDCWD, "/dev/newnode", O_CREAT | O_RDWR), KT_NEG(EROFS));
	KT_EQ(kt_open(AT_FDCWD, "/tmp/a-component-name-that-is-longer-than-fifty-three-bytes", O_CREAT | O_RDWR),
	      KT_NEG(ENAMETOOLONG));
	KT_EQ(kt_open(AT_FDCWD, "/etc/motd", 0x40000000), KT_NEG(EINVAL));
	KT_EQ(kt_open(40, "x", O_RDONLY), KT_NEG(EBADF));
	KT_EQ(kt_open(1, "x", O_RDONLY), KT_NEG(ENOTDIR)); /* console is not a directory */
	/* a path without a terminating NUL inside the mapping */
	uint64_t u = kt_page();
	char fill[64];
	memset(fill, 'a', sizeof(fill));
	for (int i = 0; i < 64; i++)
		kt_uput(u + i * 64, fill, sizeof(fill));
	KT_EQ(ksys3(SYS_openat, KT_AT_FDCWD, u, O_RDONLY), KT_NEG(ENAMETOOLONG));
	/* runs into the unmapped guard page after the window */
	KT_EQ(ksys3(SYS_openat, KT_AT_FDCWD, u + PAGE_SIZE - 8, O_RDONLY), KT_NEG(EFAULT));
}

KTEST(502, vfs_mkdir_getdents_unlink)
{
	KT_EQ(ksys2(SYS_mkdirat, KT_AT_FDCWD, kt_upath("/tmp/d502")), 0);
	KT_EQ(ksys2(SYS_mkdirat, KT_AT_FDCWD, kt_upath("/tmp/d502")), KT_NEG(EEXIST));
	int64_t f = kt_open(AT_FDCWD, "/tmp/d502/a", O_CREAT | O_RDWR);
	KT_ASSERT(f >= 0);
	KT_ASSERT(kt_open(AT_FDCWD, "/tmp/d502/b", O_CREAT | O_RDWR) >= 0);
	int64_t d = kt_open(AT_FDCWD, "/tmp/d502", O_RDONLY | O_DIRECTORY);
	uint64_t u = kt_page();
	KT_EQ(ksys3(SYS_getdents, d, u, PAGE_SIZE), 4 * sizeof(struct m_dirent));
	struct m_dirent de[4];
	kt_uget(de, u, sizeof(de));
	KT_EQ(strcmp(de[0].name, "."), 0);
	KT_EQ(strcmp(de[1].name, ".."), 0);
	KT_EQ(strcmp(de[2].name, "a"), 0);
	KT_EQ(de[2].type, DT_REG);
	KT_EQ(strcmp(de[3].name, "b"), 0);
	KT_EQ(ksys3(SYS_getdents, d, u, PAGE_SIZE), 0);
	KT_EQ(ksys3(SYS_getdents, d, u, 10), KT_NEG(EINVAL));
	KT_EQ(ksys3(SYS_getdents, f, u, PAGE_SIZE), KT_NEG(ENOTDIR));
	uint64_t dir = kt_upath("/tmp/d502");
	KT_EQ(ksys3(SYS_unlinkat, KT_AT_FDCWD, dir, AT_REMOVEDIR), KT_NEG(ENOTEMPTY));
	KT_EQ(ksys3(SYS_unlinkat, KT_AT_FDCWD, dir, 0), KT_NEG(EISDIR));
	KT_EQ(ksys3(SYS_unlinkat, d, kt_upath("a"), 0), 0);
	KT_EQ(ksys3(SYS_unlinkat, d, kt_upath("b"), 0), 0);
	KT_EQ(kt_write_str(f, "still open"), 10); /* unlinked but open */
	KT_EQ(ksys3(SYS_unlinkat, KT_AT_FDCWD, dir, AT_REMOVEDIR), 0);
	KT_EQ(ksys3(SYS_unlinkat, KT_AT_FDCWD, kt_upath("/dev/null"), 0), KT_NEG(EROFS));
	KT_EQ(ksys3(SYS_unlinkat, KT_AT_FDCWD, kt_upath("/dev"), AT_REMOVEDIR), KT_NEG(EBUSY));
	KT_EQ(ksys3(SYS_unlinkat, KT_AT_FDCWD, dir, 0x1), KT_NEG(EINVAL));
}

KTEST(503, devfs_null_zero_console)
{
	char buf[16];
	int64_t z = kt_open(AT_FDCWD, "/dev/zero", O_RDONLY);
	memset(buf, 7, sizeof(buf));
	KT_EQ(kt_read(z, buf, sizeof(buf)), 16);
	for (int i = 0; i < 16; i++)
		KT_EQ(buf[i], 0);
	int64_t n = kt_open(AT_FDCWD, "/dev/null", O_RDWR);
	KT_EQ(kt_write_str(n, "discard"), 7);
	KT_EQ(kt_read(n, buf, 4), 0);
	KT_EQ(kt_write_str(1, "    (sys_vfs: hello from a process via /dev/console)\n"), 53);
	struct m_stat st;
	uint64_t u = kt_page();
	KT_EQ(ksys2(SYS_fstat, 1, u), 0);
	kt_uget(&st, u, sizeof(st));
	KT_EQ(st.mode & S_IFMT, S_IFCHR);
}

KTEST(504, pipe_eof_epipe_and_partial_write)
{
	int64_t r, w;
	char buf[8] = { 0 };
	KT_EQ(kt_pipe(&r, &w), 0);
	KT_EQ(kt_write_str(w, "ping"), 4);
	KT_EQ(kt_read(r, buf, 2), 2);
	KT_EQ(kt_read(r, buf, 8), 2);
	KT_EQ(memcmp(buf, "ng", 2), 0);
	KT_EQ(ksys3(SYS_lseek, r, 0, SEEK_SET), KT_NEG(ESPIPE));
	KT_EQ(kt_read(w, buf, 1), KT_NEG(EBADF));
	/* partial write: 4 KiB buffer */
	uint64_t big = kt_umem(2 * PAGE_SIZE);
	KT_EQ(ksys3(SYS_write, w, big, 5000), 4096);
	KT_EQ(ksys3(SYS_read, r, big, 5000), 4096);
	/* EOF once the writer is gone */
	KT_EQ(ksys1(SYS_close, w), 0);
	KT_EQ(kt_read(r, buf, 8), 0);
	/* EPIPE once the reader is gone */
	KT_EQ(kt_pipe(&r, &w), 0);
	KT_EQ(ksys1(SYS_close, r), 0);
	KT_EQ(kt_write_str(w, "x"), KT_NEG(EPIPE));
}

KTEST(505, fd_table_exhaustion_is_emfile)
{
	for (int i = 3; i < 64; i++)
		KT_EQ(ksys1(SYS_dup, 0), i);
	KT_EQ(ksys1(SYS_dup, 0), KT_NEG(EMFILE));
	KT_EQ(kt_open(AT_FDCWD, "/etc/motd", O_RDONLY), KT_NEG(EMFILE));
	KT_EQ(ksys1(SYS_kqueue, 0), KT_NEG(EMFILE));
	KT_EQ(ksys1(SYS_close, 10), 0);
	KT_EQ(ksys1(SYS_dup, 0), 10);
}

static void exit_after(void *arg)
{
	ksys1(SYS_nanosleep, 30000000ULL);
	ksys1(SYS_exit, (uint64_t)arg);
}

KTEST(506, waitpid_sleeps_until_child_exits)
{
	uint64_t t0 = ticks;
	int pid = kt_spawn("child506", exit_after, (void *)7);
	KT_ASSERT(pid > 0);
	uint64_t st = kt_page();
	KT_EQ(ksys3(SYS_waitpid, pid, st, WNOHANG), 0); /* still running */
	KT_EQ(ksys3(SYS_waitpid, (uint64_t)-1, st, 0), pid);
	KT_EQ((int32_t)kt_get_u32(st), 7);
	KT_ASSERT(ticks - t0 >= 25);
	KT_EQ(ksys3(SYS_waitpid, (uint64_t)-1, st, 0), KT_NEG(ECHILD));
	KT_EQ(ksys3(SYS_waitpid, 0, st, 0), KT_NEG(EINVAL));
	KT_EQ(ksys0(SYS_getppid), 0); /* test processes are children of pid 0 */
}

KTEST(507, nanosleep_and_yield)
{
	uint64_t t0 = ticks;
	KT_EQ(ksys1(SYS_nanosleep, 20000000ULL), 0);
	KT_ASSERT(ticks - t0 >= 20);
	KT_EQ(ksys0(SYS_sched_yield), 0);
	KT_EQ(ksys1(SYS_nanosleep, 0), 0);
}
