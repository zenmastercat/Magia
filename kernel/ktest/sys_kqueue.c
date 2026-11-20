/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* sys_kqueue.c - kqueue/kevent filters and blocking (6xx). */
#include "kt_sys.h"

#define KQ_MAX 8

struct kq {
	int64_t fd;
	uint64_t changes, events;
};

static void kq_open(struct kq *q)
{
	q->fd = ksys0(SYS_kqueue);
	KT_ASSERT(q->fd >= 3);
	q->changes = kt_page();
	q->events = kt_page();
}

/* Apply changes (may be NULL) and collect up to KQ_MAX events. */
static int64_t kq_run(struct kq *q, const struct kevent *ch, int nch, struct kevent *out, int64_t timeout_ms)
{
	if (nch)
		kt_uput(q->changes, ch, (size_t)nch * sizeof(*ch));
	int64_t r = ksys6(SYS_kevent, q->fd, q->changes, nch, q->events, KQ_MAX, (uint64_t)timeout_ms);
	if (r > 0)
		kt_uget(out, q->events, (size_t)r * sizeof(*out));
	return r;
}

static struct kevent ev(uint64_t ident, int16_t filter, uint16_t flags, uint32_t fflags, int64_t data)
{
	struct kevent e = { ident, filter, flags, fflags, data, 0x600 };
	return e;
}

KTEST(600, kqueue_pipe_read_write_readiness)
{
	struct kq q;
	struct kevent out[KQ_MAX];
	int64_t r, w;
	kq_open(&q);
	KT_EQ(kt_pipe(&r, &w), 0);
	struct kevent ch[2] = { ev(r, EVFILT_READ, EV_ADD, 0, 0), ev(w, EVFILT_WRITE, EV_ADD, 0, 0) };
	KT_EQ(kq_run(&q, ch, 2, out, 0), 1); /* only writable */
	KT_EQ(out[0].filter, EVFILT_WRITE);
	KT_EQ(out[0].data, 4096);
	KT_EQ(out[0].udata, 0x600);
	KT_EQ(kt_write_str(w, "abc"), 3);
	KT_EQ(kq_run(&q, NULL, 0, out, 0), 2);
	KT_EQ(out[0].filter, EVFILT_READ);
	KT_EQ(out[0].data, 3);
	KT_EQ(ksys1(SYS_close, w), 0);
	KT_EQ(kq_run(&q, NULL, 0, out, 0), 1);
	KT_ASSERT(out[0].flags & EV_EOF);
	struct kevent del = ev(r, EVFILT_READ, EV_DELETE, 0, 0);
	KT_EQ(kq_run(&q, &del, 1, out, 0), 0);
}

KTEST(601, kqueue_timer_wakes_blocked_kevent)
{
	struct kq q;
	struct kevent out[KQ_MAX];
	kq_open(&q);
	struct kevent t = ev(1, EVFILT_TIMER, EV_ADD, 0, 20);
	uint64_t t0 = ticks;
	KT_EQ(kq_run(&q, &t, 1, out, -1), 1); /* blocks ~20 ms */
	KT_ASSERT(ticks - t0 >= 15);
	KT_EQ(out[0].ident, 1);
	KT_ASSERT(out[0].data >= 1);
	/* periodic: fires again */
	KT_EQ(kq_run(&q, NULL, 0, out, 1000), 1);
	struct kevent del = ev(1, EVFILT_TIMER, EV_DELETE, 0, 0);
	KT_EQ(kq_run(&q, &del, 1, out, 0), 0);
	/* timeout with nothing ready returns 0 */
	t0 = ticks;
	KT_EQ(kq_run(&q, NULL, 0, out, 30), 0);
	KT_ASSERT(ticks - t0 >= 25);
}

KTEST(602, kqueue_user_events)
{
	struct kq q;
	struct kevent out[KQ_MAX];
	kq_open(&q);
	struct kevent add = ev(42, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0);
	struct kevent trig = ev(42, EVFILT_USER, 0, NOTE_TRIGGER, 0);
	KT_EQ(kq_run(&q, &add, 1, out, 0), 0);
	KT_EQ(kq_run(&q, &trig, 1, out, 0), 1);
	KT_EQ(out[0].ident, 42);
	KT_EQ(out[0].filter, EVFILT_USER);
	KT_EQ(kq_run(&q, NULL, 0, out, 0), 0); /* EV_CLEAR reset it */
}

static void exit5(void *arg)
{
	(void)arg;
	ksys1(SYS_nanosleep, 20000000ULL);
	ksys1(SYS_exit, 5);
}

KTEST(603, kqueue_proc_exit_from_another_process)
{
	struct kq q;
	struct kevent out[KQ_MAX];
	kq_open(&q);
	int pid = kt_spawn("child603", exit5, NULL);
	KT_ASSERT(pid > 0);
	struct kevent p = ev((uint64_t)pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0);
	KT_EQ(kq_run(&q, &p, 1, out, 5000), 1);
	KT_EQ(out[0].ident, pid);
	KT_EQ(out[0].fflags, NOTE_EXIT);
	KT_EQ(out[0].data, 5);
	KT_EQ(kt_wait(pid), 5);
}

KTEST(604, kqueue_change_errors)
{
	struct kq q;
	struct kevent out[KQ_MAX];
	kq_open(&q);
	struct kevent bad[5] = {
		ev(1, EVFILT_TIMER, EV_ADD, 0, 0),           /* period 0 */
		ev(99999, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0), /* no such process */
		ev(50, EVFILT_READ, EV_ADD, 0, 0),           /* not an fd */
		ev(1, -42, EV_ADD, 0, 0),                    /* unknown filter */
		ev(7, EVFILT_USER, EV_DELETE, 0, 0),         /* not registered */
	};
	KT_EQ(kq_run(&q, bad, 5, out, 0), 5);
	KT_EQ(out[0].data, EINVAL);
	KT_EQ(out[1].data, ESRCH);
	KT_EQ(out[2].data, EBADF);
	KT_EQ(out[3].data, EINVAL);
	KT_EQ(out[4].data, ENOENT);
	for (int i = 0; i < 5; i++)
		KT_ASSERT(out[i].flags & EV_ERROR);
	/* no room for the error event: the call itself fails */
	kt_uput(q.changes, bad, sizeof(bad[0]));
	KT_EQ(ksys6(SYS_kevent, q.fd, q.changes, 1, q.events, 0, 0), KT_NEG(EINVAL));
	KT_EQ(ksys6(SYS_kevent, q.fd, q.changes, 65, q.events, 0, 0), KT_NEG(EINVAL));
	KT_EQ(ksys6(SYS_kevent, 1, 0, 0, q.events, 1, 0), KT_NEG(EBADF)); /* not a kqueue */
	/* Capsicum: kqueue rights and CAP_EVENT on the watched fd */
	int64_t r, w;
	KT_EQ(kt_pipe(&r, &w), 0);
	KT_EQ(ksys2(SYS_cap_rights_limit, r, CAP_READ), 0);
	struct kevent rd = ev(r, EVFILT_READ, EV_ADD, 0, 0);
	KT_EQ(kq_run(&q, &rd, 1, out, 0), 1);
	KT_EQ(out[0].data, ENOTCAPABLE);
	KT_EQ(ksys2(SYS_cap_rights_limit, q.fd, CAP_KQUEUE_EVENT), 0);
	struct kevent u = ev(1, EVFILT_USER, EV_ADD, 0, 0);
	KT_EQ(kq_run(&q, &u, 1, out, 0), KT_NEG(ENOTCAPABLE));
}

/* Security review F2: a kqueue holds at most 1024 notes (kernel heap that
 * no jail is charged for), and TIMER/USER idents must fit in 31 bits. */
KTEST(605, kqueue_note_count_is_bounded)
{
	struct kq q;
	struct kevent out[KQ_MAX];
	static struct kevent batch[64];
	kq_open(&q);
	for (uint64_t base = 0; base < 1024; base += 64) {
		for (int i = 0; i < 64; i++)
			batch[i] = ev(base + (uint64_t)i, EVFILT_USER, EV_ADD, 0, 0);
		KT_EQ(kq_run(&q, batch, 64, out, 0), 0);
	}
	struct kevent extra = ev(5000, EVFILT_USER, EV_ADD, 0, 0);
	KT_EQ(kq_run(&q, &extra, 1, out, 0), 1);
	KT_ASSERT(out[0].flags & EV_ERROR);
	KT_EQ(out[0].data, ENOMEM);
	struct kevent del = ev(7, EVFILT_USER, EV_DELETE, 0, 0);
	KT_EQ(kq_run(&q, &del, 1, out, 0), 0);
	KT_EQ(kq_run(&q, &extra, 1, out, 0), 0); /* a freed slot can be reused */
	struct kevent big = ev(1ULL << 40, EVFILT_TIMER, EV_ADD, 0, 10);
	KT_EQ(kq_run(&q, &big, 1, out, 0), 1);
	KT_EQ(out[0].data, EINVAL);
	KT_EQ(ksys1(SYS_close, q.fd), 0); /* frees all notes */
}
