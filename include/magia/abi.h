/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * abi.h - the Magia system call ABI (mirrored in rust/src/abi.rs).
 *
 * Calling convention (x86_64 `syscall`): rax = number, arguments in
 * rdi, rsi, rdx, r10, r8, r9; result in rax. Errors are returned as
 * -errno (Linux style). rcx and r11 are clobbered by the CPU.
 * docs/SYSCALLS.md documents every call.
 */
#ifndef MAGIA_ABI_H
#define MAGIA_ABI_H

#include <stdint.h>

/* ---- system call numbers ---- */
#define SYS_exit             1
#define SYS_getpid           2
#define SYS_getppid          3
#define SYS_read             4
#define SYS_write            5
#define SYS_openat           6
#define SYS_close            7
#define SYS_lseek            8
#define SYS_mkdirat          9
#define SYS_unlinkat        10
#define SYS_getdents        11
#define SYS_fstat           12
#define SYS_dup             13
#define SYS_pipe            14
#define SYS_sched_yield     20
#define SYS_nanosleep       21
#define SYS_sched_setweight 22
#define SYS_setpriority     23
#define SYS_procinfo        24
#define SYS_pslist          25
#define SYS_waitpid         26
#define SYS_cap_enter       30
#define SYS_cap_getmode     31
#define SYS_cap_rights_limit 32
#define SYS_cap_rights_get  33
#define SYS_jail_create     40
#define SYS_jail_attach     41
#define SYS_jail_get        42
#define SYS_gethostname     43
#define SYS_sethostname     44
#define SYS_jail_set        45
#define SYS_kqueue          50
#define SYS_kevent          51
#define SYS_uring_setup     60
#define SYS_uring_enter     61
#define SYS_futex           70
#define SYS_sysctl          80
#define SYS_uname           81
#define SYS_mmap            90
#define SYS_munmap          91
#define SYS_MAX             92

/* ---- errno (POSIX values; ENOTCAPABLE/ECAPMODE as in FreeBSD) ---- */
#define EPERM         1
#define ENOENT        2
#define ESRCH         3
#define EINTR         4
#define EIO           5
#define E2BIG         7
#define EBADF         9
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define EBUSY        16
#define EEXIST       17
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define EMFILE       24
#define EFBIG        27
#define ENOSPC       28
#define ESPIPE       29
#define EROFS        30
#define EPIPE        32
#define ERANGE       34
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39
#define ETIME        62
#define ENOTCAPABLE  93
#define ECAPMODE     94
#define ETIMEDOUT   110

/* ---- open(2) ---- */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_APPEND    0x0008
#define O_CREAT     0x0200
#define O_TRUNC     0x0400
#define O_EXCL      0x0800
#define O_DIRECTORY 0x20000
#define AT_FDCWD    (-100)
#define AT_REMOVEDIR 0x800
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define S_IFMT  0170000
#define S_IFIFO 0010000
#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFREG 0100000

struct m_stat {
	uint64_t ino;
	uint64_t size;
	uint32_t mode;
	uint32_t nlink;
	uint32_t dev;
	uint32_t _pad;
};

#define DT_FIFO 1
#define DT_CHR  2
#define DT_DIR  4
#define DT_REG  8
#define DIRENT_NAME_MAX 54
struct m_dirent {
	uint64_t ino;
	uint8_t type;
	uint8_t namelen;
	char name[DIRENT_NAME_MAX];
};

/* ---- Capsicum rights (one 64-bit mask, simplified from cap_rights_t) ---- */
#define CAP_READ          (1ULL << 0)
#define CAP_WRITE         (1ULL << 1)
#define CAP_SEEK          (1ULL << 2)
#define CAP_MMAP          (1ULL << 3)
#define CAP_EVENT         (1ULL << 4)
#define CAP_FSTAT         (1ULL << 5)
#define CAP_LOOKUP        (1ULL << 6)
#define CAP_CREATE        (1ULL << 7)
#define CAP_MKDIRAT       (1ULL << 8)
#define CAP_UNLINKAT      (1ULL << 9)
#define CAP_KQUEUE_CHANGE (1ULL << 10)
#define CAP_KQUEUE_EVENT  (1ULL << 11)
#define CAP_URING         (1ULL << 12)
#define CAP_FTRUNCATE     (1ULL << 13)
#define CAP_ALL           ((1ULL << 14) - 1)
#define CAP_PREAD  (CAP_READ | CAP_SEEK)
#define CAP_PWRITE (CAP_WRITE | CAP_SEEK)

/* ---- jails ---- */
#define JAIL_ALLOW_SET_HOSTNAME 0x1
#define JAIL_ALLOW_RING         0x2 /* may use uring_setup; jail 0 has it */
struct jail_params {
	uint64_t path;      /* user pointer, NUL-terminated, resolved in creator's root */
	uint64_t hostname;  /* user pointer, NUL-terminated */
	uint64_t mem_max;   /* bytes; 0 = inherit parent's limit */
	uint32_t cpu_weight;/* 1..10000, 0 = default (100) */
	uint32_t children_max;
	uint32_t pids_max;  /* 0 = inherit */
	uint32_t flags;
};
#define JAIL_HOSTNAME_MAX 64
#define JAIL_PATH_MAX 128
struct jail_info {
	int32_t jid;
	int32_t parent;
	uint32_t cpu_weight;
	uint32_t nprocs;
	uint64_t mem_max;
	uint64_t mem_cur;
	uint32_t children_max;
	uint32_t children_cur;
	uint32_t pids_max;
	uint32_t depth;
	char hostname[JAIL_HOSTNAME_MAX];
	char path[JAIL_PATH_MAX];
};
/* jail_set(jid, what, value) */
#define JAIL_SET_CPU_WEIGHT 1
#define JAIL_SET_MEM_MAX    2
#define JAIL_SET_PIDS_MAX   3

/* ---- processes ---- */
#define PROC_NAME_MAX 32
struct proc_info {
	int32_t pid;
	int32_t ppid;
	int32_t jid;
	int32_t tid;
	uint32_t state; /* 0 running/runnable, 1 sleeping, 2 zombie */
	int32_t nice;
	uint32_t weight;
	uint32_t cap_mode;
	uint64_t runtime_ns;
	uint64_t mem_bytes;
	char name[PROC_NAME_MAX];
};
#define WNOHANG 1

struct utsname {
	char sysname[32];
	char nodename[64];
	char release[32];
	char version[64];
	char machine[32];
};

/* ---- kqueue ---- */
struct kevent {
	uint64_t ident;
	int16_t filter;
	uint16_t flags;
	uint32_t fflags;
	int64_t data;
	uint64_t udata;
};
#define EVFILT_READ   (-1)
#define EVFILT_WRITE  (-2)
#define EVFILT_PROC   (-5)
#define EVFILT_TIMER  (-7)
#define EVFILT_USER   (-11)
#define EV_ADD      0x0001
#define EV_DELETE   0x0002
#define EV_ENABLE   0x0004
#define EV_DISABLE  0x0008
#define EV_ONESHOT  0x0010
#define EV_CLEAR    0x0020
#define EV_RECEIPT  0x0040
#define EV_ERROR    0x4000
#define EV_EOF      0x8000
#define NOTE_TRIGGER 0x01000000
#define NOTE_EXIT    0x80000000

/* ---- io_uring-style rings ---- */
#define URING_OP_NOP     0
#define URING_OP_READ    1
#define URING_OP_WRITE   2
#define URING_OP_TIMEOUT 3
#define URING_MAX_ENTRIES 256
#define URING_OFF_CURRENT 0xffffffffffffffffULL
struct uring_sqe {
	uint8_t opcode;
	uint8_t flags;
	uint16_t _pad;
	int32_t fd;
	uint64_t off;  /* file offset, or ms for TIMEOUT */
	uint64_t addr; /* user buffer */
	uint32_t len;
	uint32_t _pad2;
	uint64_t user_data;
};
struct uring_cqe {
	uint64_t user_data;
	int32_t res;
	uint32_t flags;
};
/* Fixed shared-ring header layout (offsets from ring base) */
struct uring_ring_hdr {
	uint32_t sq_head, sq_tail, sq_mask, sq_entries, sq_dropped, sq_flags;
	uint32_t _r0[2];
	uint32_t cq_head, cq_tail, cq_mask, cq_entries, cq_overflow;
	uint32_t _r1[3];
};
struct uring_params {
	uint32_t sq_entries;
	uint32_t cq_entries;
	uint64_t ring_addr;   /* user address of the shared region */
	uint64_t ring_size;
	uint64_t sq_array_off;/* u32 index array */
	uint64_t sqes_off;
	uint64_t cqes_off;
};

/* ---- mmap ---- */
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4 /* refused: user code is only mapped by the kernel */
#define MMAP_MAX_BYTES (16ULL << 20)

/* ---- futex ---- */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

#endif
