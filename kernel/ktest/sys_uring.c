/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* sys_uring.c - io_uring-style rings and futex (7xx). */
#include "kt_sys.h"

struct ring {
	int64_t fd;
	struct uring_params p;
};

#define HDR_OFF(field) ((uint64_t)__builtin_offsetof(struct uring_ring_hdr, field))

static int64_t ring_setup(struct ring *r, uint32_t entries)
{
	uint64_t u = kt_page();
	r->fd = ksys2(SYS_uring_setup, entries, u);
	if (r->fd >= 0)
		kt_uget(&r->p, u, sizeof(r->p));
	return r->fd;
}

static uint32_t hdr_get(struct ring *r, uint64_t off)
{
	return kt_get_u32(r->p.ring_addr + off);
}

static void hdr_set(struct ring *r, uint64_t off, uint32_t v) { kt_uput(r->p.ring_addr + off, &v, sizeof(v)); }

/* Queue one SQE at the tail; `index` normally equals the slot. */
static void ring_push_idx(struct ring *r, const struct uring_sqe *s, uint32_t index)
{
	uint32_t tail = hdr_get(r, HDR_OFF(sq_tail));
	uint32_t slot = tail & (r->p.sq_entries - 1);
	kt_uput(r->p.ring_addr + r->p.sqes_off + slot * sizeof(*s), s, sizeof(*s));
	kt_uput(r->p.ring_addr + r->p.sq_array_off + slot * 4, &index, 4);
	hdr_set(r, HDR_OFF(sq_tail), tail + 1);
}

static void ring_push(struct ring *r, const struct uring_sqe *s)
{
	uint32_t slot = hdr_get(r, HDR_OFF(sq_tail)) & (r->p.sq_entries - 1);
	ring_push_idx(r, s, slot);
}

/* Pop one CQE; returns false if the completion queue is empty. */
static bool ring_pop(struct ring *r, struct uring_cqe *c)
{
	uint32_t head = hdr_get(r, HDR_OFF(cq_head));
	if (head == hdr_get(r, HDR_OFF(cq_tail)))
		return false;
	kt_uget(c, r->p.ring_addr + r->p.cqes_off + (head & (r->p.cq_entries - 1)) * sizeof(*c), sizeof(*c));
	hdr_set(r, HDR_OFF(cq_head), head + 1);
	return true;
}

static struct uring_sqe sqe(uint8_t op, int32_t fd, uint64_t off, uint64_t addr, uint32_t len, uint64_t ud)
{
	struct uring_sqe s = { .opcode = op, .fd = fd, .off = off, .addr = addr, .len = len, .user_data = ud };
	return s;
}

KTEST(700, uring_nop_write_read)
{
	struct ring r;
	struct uring_cqe c;
	KT_ASSERT(ring_setup(&r, 8) >= 3);
	KT_EQ(r.p.sq_entries, 8);
	KT_EQ(r.p.cq_entries, 16);
	KT_EQ(hdr_get(&r, HDR_OFF(sq_mask)), 7);
	int64_t f = kt_open(AT_FDCWD, "/tmp/ring700", O_CREAT | O_RDWR);
	uint64_t buf = kt_page(), out = kt_page();
	kt_uput(buf, "ring data", 9);
	struct uring_sqe s1 = sqe(URING_OP_NOP, 0, 0, 0, 0, 1);
	struct uring_sqe s2 = sqe(URING_OP_WRITE, (int32_t)f, 0, buf, 9, 2);
	ring_push(&r, &s1);
	ring_push(&r, &s2);
	KT_EQ(ksys4(SYS_uring_enter, r.fd, 2, 2, 0), 2);
	KT_ASSERT(ring_pop(&r, &c));
	KT_EQ(c.user_data, 1);
	KT_EQ(c.res, 0);
	KT_ASSERT(ring_pop(&r, &c));
	KT_EQ(c.user_data, 2);
	KT_EQ(c.res, 9);
	struct uring_sqe s3 = sqe(URING_OP_READ, (int32_t)f, 5, out, 64, 3);
	ring_push(&r, &s3);
	KT_EQ(ksys4(SYS_uring_enter, r.fd, 1, 1, 0), 1);
	KT_ASSERT(ring_pop(&r, &c));
	KT_EQ(c.res, 4);
	char got[5] = { 0 };
	kt_uget(got, out, 4);
	KT_EQ(strcmp(got, "data"), 0);
	KT_ASSERT(!ring_pop(&r, &c));
	/* the ring fd is closed like any other descriptor */
	KT_EQ(ksys1(SYS_close, r.fd), 0);
}

KTEST(701, uring_rejects_bad_sqes_and_indexes)
{
	struct ring r;
	struct uring_cqe c;
	uint64_t p = kt_page();
	KT_EQ(ksys2(SYS_uring_setup, 3, p), KT_NEG(EINVAL));
	KT_EQ(ksys2(SYS_uring_setup, 512, p), KT_NEG(EINVAL));
	KT_EQ(ksys2(SYS_uring_setup, 4, 0), KT_NEG(EFAULT));
	KT_ASSERT(ring_setup(&r, 4) >= 3);
	struct uring_sqe bad_op = sqe(77, 0, 0, 0, 0, 10);
	struct uring_sqe bad_flags = sqe(URING_OP_NOP, 0, 0, 0, 0, 11);
	bad_flags.flags = 1;
	struct uring_sqe kaddr = sqe(URING_OP_WRITE, 1, URING_OFF_CURRENT, KERNEL_VMA, 8, 12);
	struct uring_sqe badfd = sqe(URING_OP_READ, 40, URING_OFF_CURRENT, p, 8, 13);
	ring_push(&r, &bad_op);
	ring_push(&r, &bad_flags);
	ring_push(&r, &kaddr);
	ring_push(&r, &badfd);
	KT_EQ(ksys4(SYS_uring_enter, r.fd, 4, 0, 0), 4);
	int32_t want[4] = { -EINVAL, -EINVAL, -EFAULT, -EBADF };
	for (int i = 0; i < 4; i++) {
		KT_ASSERT(ring_pop(&r, &c));
		KT_EQ(c.user_data, 10 + i);
		KT_EQ(c.res, want[i]);
	}
	/* corrupted SQ index array: dropped, never executed */
	struct uring_sqe nop = sqe(URING_OP_NOP, 0, 0, 0, 0, 20);
	ring_push_idx(&r, &nop, 1000);
	KT_EQ(ksys4(SYS_uring_enter, r.fd, 1, 0, 0), 1);
	KT_EQ(hdr_get(&r, HDR_OFF(sq_dropped)), 1);
	KT_ASSERT(!ring_pop(&r, &c));
	/* corrupted tail: bounded to one ring's worth */
	hdr_set(&r, HDR_OFF(sq_tail), 0xdeadbeef);
	int64_t n = ksys4(SYS_uring_enter, r.fd, 256, 0, 0);
	KT_ASSERT(n >= 0 && n <= 4);
	/* not a ring / bad flags */
	KT_EQ(ksys4(SYS_uring_enter, 0, 0, 0, 0), KT_NEG(EINVAL));
	KT_EQ(ksys4(SYS_uring_enter, r.fd, 0, 0, 1), KT_NEG(EINVAL));
}

KTEST(702, uring_timeout_blocks_until_timer)
{
	struct ring r;
	struct uring_cqe c;
	KT_ASSERT(ring_setup(&r, 4) >= 3);
	struct uring_sqe t = sqe(URING_OP_TIMEOUT, 0, 25, 0, 0, 99);
	ring_push(&r, &t);
	uint64_t t0 = ticks;
	KT_EQ(ksys4(SYS_uring_enter, r.fd, 1, 1, 0), 1);
	KT_ASSERT(ticks - t0 >= 20);
	KT_ASSERT(ring_pop(&r, &c));
	KT_EQ(c.user_data, 99);
	KT_EQ(c.res, -ETIME);
	/* waiting with nothing in flight returns at once */
	KT_EQ(ksys4(SYS_uring_enter, r.fd, 0, 3, 0), 0);
}

KTEST(703, uring_kill_switch_and_jail_permission)
{
	struct ring r;
	uint64_t p = kt_page();
	uint64_t was = 0;
	KT_EQ(kt_sysctl_u64("kern.ring.enable", &was), 0);
	KT_EQ(was, 1);
	KT_ASSERT(ring_setup(&r, 4) >= 3);
	KT_EQ(kt_sysctl_set_u64("kern.ring.enable", 0), 0);
	KT_EQ(ksys2(SYS_uring_setup, 4, p), KT_NEG(EPERM));
	KT_EQ(ksys4(SYS_uring_enter, r.fd, 0, 0, 0), KT_NEG(EPERM));
	KT_EQ(kt_sysctl_set_u64("kern.ring.enable", 1), 0);
	KT_EQ(kt_sysctl_set_u64("kern.ring.enable", 2), KT_NEG(EINVAL));
	KT_EQ(ksys4(SYS_uring_enter, r.fd, 0, 0, 0), 0);
	/* jails without JAIL_ALLOW_RING cannot create rings */
	int64_t noring = kt_jail("/", "noring", 0, 0, 0, 0);
	int64_t ring = kt_jail("/", "ring", 0, 0, 0, JAIL_ALLOW_RING);
	KT_ASSERT(noring > 0 && ring > 0);
	KT_EQ(ksys1(SYS_jail_attach, ring), 0);
	int64_t fd = ksys2(SYS_uring_setup, 4, p);
	KT_ASSERT(fd >= 0);
	KT_EQ(kt_sysctl_set_u64("kern.ring.enable", 0), KT_NEG(EPERM)); /* not from a jail */
	struct jail_params jp = { kt_upath("/"), kt_upath("inner"), 0, 0, 4, 0, 0 };
	uint64_t ujp = kt_page();
	kt_uput(ujp, &jp, sizeof(jp));
	int64_t inner = ksys1(SYS_jail_create, ujp); /* flag not requested: not inherited */
	KT_ASSERT(inner > 0);
	KT_EQ(ksys1(SYS_jail_attach, inner), 0);
	KT_EQ(ksys2(SYS_uring_setup, 4, p), KT_NEG(EPERM));
}

KTEST(704, uring_ops_share_capsicum_checks)
{
	struct ring r;
	struct uring_cqe c;
	KT_ASSERT(ring_setup(&r, 4) >= 3);
	int64_t f = kt_open(AT_FDCWD, "/etc/motd", O_RDONLY);
	uint64_t buf = kt_page();
	KT_EQ(ksys2(SYS_cap_rights_limit, f, CAP_READ), 0);
	struct uring_sqe ok = sqe(URING_OP_READ, (int32_t)f, URING_OFF_CURRENT, buf, 7, 1);
	struct uring_sqe pread = sqe(URING_OP_READ, (int32_t)f, 0, buf, 7, 2); /* needs CAP_SEEK */
	struct uring_sqe wr = sqe(URING_OP_WRITE, (int32_t)f, URING_OFF_CURRENT, buf, 7, 3);
	ring_push(&r, &ok);
	ring_push(&r, &pread);
	ring_push(&r, &wr);
	KT_EQ(ksys4(SYS_uring_enter, r.fd, 3, 3, 0), 3);
	int32_t want[3] = { 7, -ENOTCAPABLE, -ENOTCAPABLE };
	for (int i = 0; i < 3; i++) {
		KT_ASSERT(ring_pop(&r, &c));
		KT_EQ(c.res, want[i]);
	}
	/* CAP_URING on the ring fd itself */
	KT_EQ(ksys2(SYS_cap_rights_limit, r.fd, CAP_READ), 0);
	KT_EQ(ksys4(SYS_uring_enter, r.fd, 0, 0, 0), KT_NEG(ENOTCAPABLE));
}

KTEST(710, futex_wait_times_out)
{
	uint64_t w = kt_page();
	uint32_t zero = 0;
	kt_uput(w, &zero, 4);
	uint64_t t0 = ticks;
	KT_EQ(ksys4(SYS_futex, w, FUTEX_WAIT, 0, 30), KT_NEG(ETIMEDOUT));
	KT_ASSERT(ticks - t0 >= 25);
	KT_EQ(ksys4(SYS_futex, w, FUTEX_WAIT, 1, 30), KT_NEG(EAGAIN));
	KT_EQ(ksys4(SYS_futex, w + 2, FUTEX_WAIT, 0, 30), KT_NEG(EINVAL));
	KT_EQ(ksys4(SYS_futex, w, 7, 0, 0), KT_NEG(EINVAL));
	KT_EQ(ksys4(SYS_futex, KERNEL_VMA, FUTEX_WAKE, 1, 0), KT_NEG(EFAULT));
	KT_EQ(ksys4(SYS_futex, w, FUTEX_WAKE, 5, 0), 0); /* nobody waiting */
}

static void futex_waker(void *arg)
{
	uint64_t w = (uint64_t)arg;
	uint32_t one = 1;
	ksys1(SYS_nanosleep, 20000000ULL);
	kt_uput(w, &one, 4);
	int64_t n = ksys4(SYS_futex, w, FUTEX_WAKE, 1, 0);
	ksys1(SYS_exit, (uint64_t)n);
}

KTEST(711, futex_wake_from_another_process)
{
	uint64_t w = kt_page();
	uint32_t zero = 0;
	kt_uput(w, &zero, 4);
	int pid = kt_spawn("waker", futex_waker, (void *)w);
	KT_ASSERT(pid > 0);
	uint64_t t0 = ticks;
	int64_t r = ksys4(SYS_futex, w, FUTEX_WAIT, 0, 5000);
	int woken = kt_wait(pid);
	if (r == 0) {
		KT_EQ(woken, 1); /* the child woke exactly us */
		KT_ASSERT(ticks - t0 < 5000);
	} else {
		/* the child ran first: the word already changed */
		KT_EQ(r, KT_NEG(EAGAIN));
		KT_EQ(woken, 0);
	}
	KT_EQ(kt_get_u32(w), 1);
}
