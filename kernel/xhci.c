/* SPDX-License-Identifier: ISC AND BSD-2-Clause */
/*
 * Copyright (c) 2026 zenmastercat and Magia contributors
 *
 * Portions adapted from OpenBSD sys/dev/usb/xhci.c (xhci_reset,
 * xhci_init/xhci_config register sequence, xhci_ring_reset/_consume/
 * _produce, xhci_event_dequeue, the route string and TT fields of
 * xhci_context_setup), OpenBSD src commit 8e1a11b4:
 *
 * Copyright (c) 2014-2015 Martin Pieuchot
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * and from OpenBSD sys/dev/pci/xhci_pci.c (xhci_pci_takecontroller,
 * xhci_pci_port_route) and sys/dev/usb/uhub.c rev 1.99 (uhub_attach,
 * uhub_explore, uhub_port_connect: the order of hub requests) and
 * usb_subr.c (the high-speed hub port above a device, `myhsport`),
 * which carry:
 *
 * Copyright (c) 1998, 2001, 2002 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson (lennart@augustsson.net) at
 * Carlstedt Research & Technology.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * xhci.c - USB 3 host controllers (xHCI 1.2), USB 2/3 hubs, HID boot
 * keyboards and mice.
 *
 * Every xHCI controller on the PCI bus is started, each with its own
 * rings, device table and MSI-X (or MSI) vector. Without OpenBSD's usbdi
 * layer, one kernel thread ("usb") does all enumeration: the first scan
 * at boot, then root-port changes (port-change events), hub port changes
 * (the hub's status-change endpoint), halted endpoints and ktest
 * requests. Commands and control transfers therefore never run
 * concurrently; the interrupt handler only consumes events, hands
 * keyboard and mouse reports to Rust, re-queues transfers and wakes the
 * thread. Interrupts off is the lock (uniprocessor): the handler and a
 * thread waiting for a completion both consume the event ring with
 * interrupts disabled.
 *
 * Hubs follow OpenBSD's uhub: hub descriptor, power every port, wait,
 * then per port: clear the change bits, detach whatever was there on a
 * connect change, reset a connected port and attach at the speed the
 * port reports. The slot context carries the route string, the hub
 * fields (Hub, number of ports, TT think time, MTT) and, for full/low
 * speed devices below a high-speed hub, the TT hub slot and port.
 *
 * Event TRB fields are checked (type, slot, endpoint, completion code,
 * residual length; pointers are only compared with our own rings, never
 * dereferenced); device-written data (descriptors, hub data, reports) is
 * parsed in Rust; the controllers' DMA is not restricted (no IOMMU, see
 * docs/ARCHITECTURE.md). Every HID boot keyboard is trusted as a console
 * keyboard.
 * A halted endpoint (review F6) gets Reset Endpoint, CLEAR_FEATURE(HALT)
 * after a stall, Set TR Dequeue Pointer and a new transfer; after 3
 * errors in a row it is given up, so a broken device cannot keep the
 * thread busy.
 * shortcut: interrupt-IN endpoints only (no bulk or isochronous), one
 * configuration, hub ports 1-15 (the route string has 4 bits per tier),
 * SuperSpeed hubs implemented from the specification but never run
 * (QEMU has none); a usbdi-like layer comes with the next class driver.
 */
#include <magia/abi.h>
#include <magia/acpi.h>
#include <magia/hwinfo.h>
#include <magia/kbd.h>
#include <magia/linker_set.h>
#include <magia/mm.h>
#include <magia/pci.h>
#include <magia/spinlock.h>
#include <magia/sysctl.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/usb.h>
#include <magia/x86.h>
#include <magia/xhcireg.h>

#define NTRB 256 /* one 4 KiB page per ring */
#define MAX_SLOTS 64
#define MAX_EPS 3    /* class endpoints per device: keyboard, mouse, a ktest one */
#define MAX_HUB_PORTS 15
#define MAX_DEPTH 5  /* hub tiers a route string can address */
#define IRQ_BATCH 32 /* events handled per interrupt */
#define EP_MAX_FAILS 3

/* hub class requests and features (USB 2.0 11.24, USB 3.2 10.16) */
#define UR_GET_DESCRIPTOR 6
#define UR_SET_INTERFACE 11
#define UR_SET_HUB_DEPTH 12
#define UHF_PORT_RESET 4
#define UHF_PORT_POWER 8
#define UHF_C_PORT_CONNECTION 16
#define UHF_C_PORT_ENABLE 17
#define UHF_C_PORT_OVER_CURRENT 19
#define UHF_C_PORT_RESET 20
#define UHF_C_PORT_LINK_STATE 25
#define UHF_C_BH_PORT_RESET 29
#define UPS_CONNECT 0x0001
#define UPS_RESET 0x0010
#define UPS_LOW_SPEED 0x0200
#define UPS_HIGH_SPEED 0x0400
#define UPS_C_CONNECT 0x0001
#define UPS_C_ENABLE 0x0002
#define UPS_C_OVERCURRENT 0x0008
#define UPS_C_RESET 0x0010
#define UPS_C_BH_RESET 0x0020 /* SuperSpeed */
#define UPS_C_LINK_STATE 0x0040 /* SuperSpeed */

struct ring {
	volatile struct xhci_trb *trbs;
	uint64_t pa;
	unsigned index;
	uint32_t toggle;
};

enum ep_kind { EP_KBD = 1, EP_MOUSE, EP_HUB, EP_TEST };

struct usb_ep {
	uint8_t kind, dci, addr, ival;
	uint16_t mps;
	struct ring ring;
	uint8_t *buf;          /* one page of transfer data */
	int kbd_id;            /* Rust keyboard state (EP_KBD) */
	bool halted;           /* error seen; the usb thread recovers it */
	bool dead;             /* given up */
	uint32_t code;         /* completion code of the last error */
	unsigned fails;        /* errors since the last good transfer */
	uint64_t xfers, errors, resets;
};

struct xhci;

struct usb_dev {
	struct usb_topo t;     /* first: usb_topo pointers of hubs lead here */
	struct xhci *hc;
	struct usb_dev *parent;
	uint8_t rootport;
	char path[24];
	uint8_t tt_slot, tt_port;
	bool mtt;              /* MTT bit of a full/low-speed device behind a multi-TT hub */
	struct rs_usb_dev desc;
	uint32_t sctx_sent[4]; /* the slot context Magia sent last */
	uint8_t *ctx;          /* output device context */
	uint64_t ctx_pa;
	struct ring ep0;
	struct usb_ep eps[MAX_EPS];
	int neps;
	/* hubs */
	bool hub, ss_hub;
	uint8_t hub_ports, hub_ttt;
	uint16_t power_on_ms;
	volatile uint32_t hub_change; /* port bitmap from the status-change endpoint */
	uint32_t hub_deferred;        /* ports waiting for the connect rate limit */
	struct usb_dev *child[MAX_HUB_PORTS + 1];
	struct usb_rate port_rate[MAX_HUB_PORTS + 1];
	bool answered;                /* the device descriptor was read */
};

struct xhci {
	struct xhci_stats *st;
	struct pci_dev *pdev;
	volatile uint8_t *cap, *op, *rt;
	volatile uint32_t *db;
	volatile uint64_t *dcbaa;
	struct ring cmd, evt;
	volatile uint32_t *erst;
	uint8_t *in_ctx;  /* input context, reused for every command */
	uint8_t *dma;     /* control transfer data */
	struct usb_dev *slots[MAX_SLOTS + 1];
	struct usb_dev **root; /* by root port, 1..nports */
	volatile uint64_t port_change[4]; /* root ports with a port-change event */
	uint64_t deferred[4];             /* root ports waiting for the connect rate limit */
	struct usb_rate *root_rate;       /* by root port */
	unsigned disable_failures;
	bool attach_off;                  /* too many Disable Slot failures: attach nothing more */
	/* the one command or control transfer the usb thread waits for */
	volatile bool wait_on, wait_done;
	uint32_t wait_type;
	uint8_t wait_slot;
	uint64_t wait_pa;
	uint32_t wait_code, wait_len;
	uint8_t wait_evslot;
};

struct evt {
	uint32_t type, code, slot, ep;
	uint64_t ptr;
	uint32_t len; /* residual length (transfer events) */
};

struct xhci_stats xhci_hc[USB_MAX_HC];
int xhci_count;
struct usb_stats usb;
static struct xhci hcs[USB_MAX_HC];
static int usb_tid = -1;
static volatile bool usb_work;
static uint32_t kbd_ids; /* Rust keyboard states in use */
static int test_fail_disable; /* ktest: treat the next Disable Slot as failed */
static void (*volatile call_fn)(void *);
static void *call_arg;
static volatile bool call_done;

SYSCTL_U64_RO(hw_usb_kbd_reports, "hw.usb.kbd_reports", &usb.kbd_reports, "USB keyboard reports received");
SYSCTL_U64_RO(hw_usb_events, "hw.usb.events", &xhci_hc[0].events, "xHCI events handled (controller 0)");

static uint32_t cread32(struct xhci *h, uint32_t o) { return *(volatile uint32_t *)(h->cap + o); }
static uint32_t oread32(struct xhci *h, uint32_t o) { return *(volatile uint32_t *)(h->op + o); }
static void owrite32(struct xhci *h, uint32_t o, uint32_t v) { *(volatile uint32_t *)(h->op + o) = v; }
static uint32_t rread32(struct xhci *h, uint32_t o) { return *(volatile uint32_t *)(h->rt + o); }
static void rwrite32(struct xhci *h, uint32_t o, uint32_t v) { *(volatile uint32_t *)(h->rt + o) = v; }

static void *dma_page(uint64_t *pa)
{
	void *p = page_alloc(0); /* zeroed, 4 KiB aligned: meets every xHCI alignment */
	if (p && pa)
		*pa = virt_to_phys(p);
	return p;
}

static bool wait_until(volatile uint32_t *reg, uint32_t mask, uint32_t want, uint64_t ms)
{
	uint64_t end = now_ns() + ms * 1000000ULL;
	while ((*reg & mask) != want) {
		if (now_ns() > end)
			return false;
		cpu_relax();
	}
	return true;
}

static void usb_kick(void)
{
	usb_work = true;
	if (usb_tid >= 0)
		thread_wake(usb_tid);
}

/* Root ports report the Protocol Speed ID: with the default PSI table
 * 1 full, 2 low, 3 high, 4 SuperSpeed (Gen 1), 5 and up SuperSpeedPlus
 * (Gen 2, Gen 1x2, Gen 2x2; Intel's 800-series ports report 5). */
const char *usb_speed_name(uint8_t speed)
{
	static const char *const n[] = { "?", "FS", "LS", "HS", "SS" };
	return speed > XHCI_SPEED_SUPER && speed < 16 ? "SS+" : n[speed < ARRAY_SIZE(n) ? speed : 0];
}

/* Connect rate limit (review 2b L5); pure, ktest 081. */
static bool rate_reset(const struct usb_rate *r, uint64_t now)
{
	return r->count == 0 || now < r->start || now - r->start >= USB_CONNECT_WINDOW_MS;
}

bool usb_connect_open(const struct usb_rate *r, uint64_t now) { return rate_reset(r, now) || r->count < USB_CONNECT_MAX; }

bool usb_connect_allowed(struct usb_rate *r, uint64_t now)
{
	if (rate_reset(r, now))
		*r = (struct usb_rate){ .start = now };
	if (r->count >= USB_CONNECT_MAX)
		return false;
	r->count++;
	return true;
}

void usb_connect_failed(struct usb_rate *r)
{
	if (r->count < USB_CONNECT_MAX)
		r->count++;
}

static void rate_deferred(struct usb_rate *r, int hc, const char *where, unsigned port)
{
	usb.connects_limited++;
	if (!r->logged) {
		r->logged = 1;
		kprintf("usb: c%d p%s%s%u: %u connects within %u s, waiting before the next\n", hc, where,
			*where ? "." : "", port, USB_CONNECT_MAX, USB_CONNECT_WINDOW_MS / 1000);
	}
}

/* ---------------------------------------------------------------- rings (OpenBSD) */

static bool ring_init(struct ring *r)
{
	r->trbs = dma_page(&r->pa);
	if (!r->trbs)
		return false;
	r->index = 0;
	r->toggle = XHCI_TRB_CYCLE;
	/* one segment: the last TRB links back to the first; ring_produce hands it over */
	volatile struct xhci_trb *t = &r->trbs[NTRB - 1];
	t->trb_paddr = r->pa;
	t->trb_flags = XHCI_TRB_TYPE_LINK | XHCI_TRB_TC;
	return true;
}

static void ring_free(struct ring *r)
{
	if (r->trbs)
		page_free((void *)r->trbs, 0);
	r->trbs = NULL;
}

static volatile struct xhci_trb *ring_consume(struct ring *r)
{
	volatile struct xhci_trb *t = &r->trbs[r->index];
	if (r->toggle != (t->trb_flags & XHCI_TRB_CYCLE))
		return NULL;
	if (++r->index == NTRB) {
		r->index = 0;
		r->toggle ^= 1;
	}
	return t;
}

/* Fill the next TRB; the cycle bit is written last so the controller never
 * sees a half-written TRB. Returns its physical address. Callers keep
 * interrupts off so two producers never interleave. */
static uint64_t ring_produce(struct ring *r, uint64_t ptr, uint32_t status, uint32_t flags)
{
	volatile struct xhci_trb *t = &r->trbs[r->index];
	uint64_t pa = r->pa + r->index * sizeof(struct xhci_trb);
	t->trb_paddr = ptr;
	t->trb_status = status;
	__asm__ volatile("" ::: "memory");
	t->trb_flags = (flags & ~XHCI_TRB_CYCLE) | r->toggle;
	if (++r->index == NTRB - 1) { /* at the link TRB: hand it over with this lap's cycle, then wrap (xHCI 4.9.2.2) */
		volatile struct xhci_trb *lnk = &r->trbs[NTRB - 1];
		lnk->trb_paddr = r->pa;
		lnk->trb_flags = XHCI_TRB_TYPE_LINK | XHCI_TRB_TC | r->toggle;
		r->index = 0;
		r->toggle ^= 1;
	}
	return pa;
}

/* Enqueue pointer with its cycle state: what Set TR Dequeue Pointer takes. */
static uint64_t ring_deq(const struct ring *r) { return (r->pa + r->index * sizeof(struct xhci_trb)) | r->toggle; }

static void doorbell(struct xhci *h, unsigned slot, uint32_t target)
{
	__asm__ volatile("mfence" ::: "memory");
	h->db[slot] = target;
}

static uint32_t *ctx(struct xhci *h, uint8_t *base, unsigned idx) { return (uint32_t *)(base + idx * h->st->csz); }

/* ---------------------------------------------------------------- events */

static bool next_event(struct xhci *h, struct evt *e)
{
	volatile struct xhci_trb *t = ring_consume(&h->evt);
	if (!t)
		return false;
	uint32_t flags = t->trb_flags, status = t->trb_status;
	e->type = flags & XHCI_TRB_TYPE_MASK;
	e->code = XHCI_TRB_GET_CODE(status);
	e->slot = XHCI_TRB_GET_SLOT(flags);
	e->ep = XHCI_TRB_GET_EP(flags);
	e->ptr = t->trb_paddr;
	e->len = XHCI_TRB_REMAIN(status);
	uint64_t dq = h->evt.pa + h->evt.index * sizeof(struct xhci_trb);
	rwrite32(h, XHCI_ERDP_LO(0), (uint32_t)dq | XHCI_ERDP_LO_BUSY);
	rwrite32(h, XHCI_ERDP_HI(0), (uint32_t)(dq >> 32));
	h->st->events++;
	return true;
}

static void ep_queue(struct usb_dev *d, struct usb_ep *ep)
{
	uint16_t len = (uint16_t)MIN(ep->mps, (uint16_t)PAGE_SIZE);
	ring_produce(&ep->ring, virt_to_phys(ep->buf), len, XHCI_TRB_TYPE_NORMAL | XHCI_TRB_IOC | XHCI_TRB_ISP);
	doorbell(d->hc, d->t.slot, ep->dci);
}

static void kbd_data(struct usb_ep *ep, uint32_t got)
{
	uint8_t out[16];
	size_t n = rs_usb_kbd_report((uint32_t)ep->kbd_id, ep->buf, got, out, sizeof(out));
	usb.kbd_reports++;
	for (size_t i = 0; i < n && i < sizeof(out); i++)
		console_rx_push(out[i]);
	usb.kbd_chars += n;
}

static void mouse_data(struct usb_ep *ep, uint32_t got)
{
	int64_t m = rs_usb_mouse_report(ep->buf, got);
	if (m < 0)
		return;
	usb.mouse_reports++;
	usb.mouse_buttons = (uint32_t)(m & 0xff);
	usb.mouse_x += (int8_t)(m >> 8);
	usb.mouse_y += (int8_t)(m >> 16);
}

/* A transfer event on a class endpoint (interrupt context or a waiter). */
static void ep_event(struct usb_dev *d, struct usb_ep *ep, const struct evt *e)
{
	if (ep->halted || ep->dead) {
		d->hc->st->stale_events++;
		return;
	}
	if (e->code == XHCI_CODE_XFER_STOPPED || e->code == XHCI_CODE_XFER_STOPINV || e->code == XHCI_CODE_XFER_SHORTPKT) {
		d->hc->st->stale_events++; /* Stop Endpoint (detach): nothing to do */
		return;
	}
	if (e->code != XHCI_CODE_SUCCESS && e->code != XHCI_CODE_SHORT_XFER) {
		/* the endpoint is halted now (stall, babble, transaction error):
		 * the usb thread resets it (review F6) */
		ep->errors++;
		ep->fails++;
		ep->code = e->code;
		ep->halted = true;
		usb.ep_errors++;
		usb_kick();
		return;
	}
	uint32_t asked = MIN(ep->mps, (uint32_t)PAGE_SIZE);
	uint32_t got = e->len <= asked ? asked - e->len : 0; /* residual from the device side: bound it */
	ep->fails = 0;
	ep->xfers++;
	switch (ep->kind) {
	case EP_KBD:
		kbd_data(ep, got);
		break;
	case EP_MOUSE:
		mouse_data(ep, got);
		break;
	case EP_HUB: { /* bit 0: the hub itself, bit n: port n */
		uint32_t bits = 0;
		for (uint32_t i = 0; i < got && i < 4; i++)
			bits |= (uint32_t)ep->buf[i] << (8 * i);
		d->hub_change |= bits;
		usb_kick();
		break;
	}
	default:
		break;
	}
	ep_queue(d, ep);
}

static void dispatch(struct xhci *h, const struct evt *e)
{
	if (e->type == XHCI_EVT_CMD_COMPLETE) {
		h->st->cmd_events++;
		if (h->wait_on && h->wait_type == XHCI_EVT_CMD_COMPLETE && e->ptr == h->wait_pa) {
			h->wait_code = e->code;
			h->wait_evslot = (uint8_t)e->slot;
			h->wait_done = true;
		}
		return;
	}
	if (e->type == XHCI_EVT_PORT_CHANGE) {
		h->st->port_events++;
		uint32_t port = (uint32_t)(e->ptr >> 24) & 0xff;
		if (port >= 1 && port <= h->st->nports) {
			h->port_change[port / 64] |= 1ULL << (port % 64);
			usb_kick();
		}
		return;
	}
	if (e->type != XHCI_EVT_XFER)
		return;
	h->st->xfer_events++;
	struct usb_dev *d = e->slot >= 1 && e->slot <= MAX_SLOTS ? h->slots[e->slot] : NULL;
	if (!d) {
		h->st->stale_events++; /* detached, or never ours */
		return;
	}
	if (e->ep == 1) { /* EP0: the control transfer the thread waits for */
		bool err = e->code != XHCI_CODE_SUCCESS && e->code != XHCI_CODE_SHORT_XFER;
		if (h->wait_on && h->wait_type == XHCI_EVT_XFER && e->slot == h->wait_slot &&
		    (e->ptr == h->wait_pa || err)) {
			/* success is reported on the status TRB; an error on
			 * whichever stage failed, so any error ends the transfer */
			h->wait_code = e->code;
			h->wait_len = e->len;
			h->wait_done = true;
		} else {
			h->st->stale_events++;
		}
		return;
	}
	for (int i = 0; i < d->neps; i++)
		if (d->eps[i].dci == e->ep) {
			ep_event(d, &d->eps[i], e);
			return;
		}
	h->st->bad_events++;
}

static unsigned process_events(struct xhci *h, unsigned max)
{
	struct evt e;
	unsigned n = 0;
	while (n < max && next_event(h, &e)) {
		n++;
		dispatch(h, &e);
	}
	return n;
}

static void xhci_irq(struct trapframe *tf)
{
	for (int i = 0; i < xhci_count; i++) {
		struct xhci *h = &hcs[i];
		if (!tf || h->st->vector != (int)tf->vector)
			continue;
		h->st->irqs++;
		rwrite32(h, XHCI_IMAN(0), rread32(h, XHCI_IMAN(0)) | XHCI_IMAN_INTR_PEND); /* RW1C */
		owrite32(h, XHCI_USBSTS, XHCI_STS_EINT);
		/* At most IRQ_BATCH events, so a device flooding the ring cannot
		 * keep the CPU here. next_event has already moved ERDP past each
		 * event and cleared EHB; with events left, the controller
		 * interrupts again. */
		unsigned n = process_events(h, IRQ_BATCH);
		h->st->irq_max_events = MAX(h->st->irq_max_events, n);
	}
}

/* ---------------------------------------------------------------- commands and control transfers */

/* Wait (usb thread) for what wait_pa names, consuming events meanwhile:
 * the interrupt handler may take the event first, either way wait_done
 * is set. Works before interrupts are on and on polled controllers. */
static bool wait_done(struct xhci *h, uint64_t ms)
{
	uint64_t end = now_ns() + ms * 1000000ULL;
	for (;;) {
		uint64_t f = irq_save();
		process_events(h, IRQ_BATCH);
		bool done = h->wait_done;
		if (done || now_ns() > end)
			h->wait_on = false;
		irq_restore(f);
		if (done)
			return true;
		if (now_ns() > end)
			return false;
		cpu_relax();
	}
}

static int command(struct xhci *h, uint64_t ptr, uint32_t status, uint32_t flags, uint8_t *slot_out)
{
	uint64_t f = irq_save();
	h->wait_done = false;
	h->wait_type = XHCI_EVT_CMD_COMPLETE;
	h->wait_pa = ring_produce(&h->cmd, ptr, status, flags);
	h->wait_on = true;
	doorbell(h, 0, 0);
	irq_restore(f);
	if (!wait_done(h, 1000))
		return -ETIMEDOUT;
	if (slot_out)
		*slot_out = h->wait_evslot;
	return h->wait_code == XHCI_CODE_SUCCESS ? 0 : -EIO;
}

static uint32_t ep_state(struct usb_dev *d, unsigned dci)
{
	return XHCI_EPCTX_STATE(*ctx(d->hc, d->ctx, dci));
}

/* After an error the endpoint is halted (or stopped): Reset Endpoint if
 * halted, then point its dequeue pointer at our enqueue position, which
 * drops whatever was left of the failed transfer (xHCI 4.6.8, 4.6.10). */
static int ep_reset(struct usb_dev *d, unsigned dci, struct ring *r)
{
	struct xhci *h = d->hc;
	uint32_t ids = XHCI_TRB_SET_SLOT(d->t.slot) | XHCI_TRB_SET_EP(dci);
	if (ep_state(d, dci) == XHCI_EP_HALTED && command(h, 0, 0, XHCI_CMD_RESET_EP | ids, NULL) != 0)
		return -EIO;
	uint64_t f = irq_save();
	uint64_t deq = ring_deq(r);
	irq_restore(f);
	return command(h, deq, 0, XHCI_CMD_SET_TR_DEQ | ids, NULL);
}

/* Control transfer on EP0: setup, optional IN data into h->dma, status. */
static int control(struct usb_dev *d, uint8_t reqtype, uint8_t req, uint16_t value, uint16_t index, uint16_t len)
{
	struct xhci *h = d->hc;
	if (len > PAGE_SIZE)
		return -EINVAL;
	uint64_t setup = (uint64_t)reqtype | (uint64_t)req << 8 | (uint64_t)value << 16 | (uint64_t)index << 32 |
			 (uint64_t)len << 48;
	bool in = len && (reqtype & 0x80);
	uint64_t f = irq_save();
	ring_produce(&d->ep0, setup, 8, XHCI_TRB_TYPE_SETUP | XHCI_TRB_IDT | (len ? (in ? XHCI_TRB_TRT_IN : XHCI_TRB_TRT_OUT) : 0));
	if (len)
		ring_produce(&d->ep0, virt_to_phys(h->dma), len, XHCI_TRB_TYPE_DATA | (in ? XHCI_TRB_DIR_IN : 0));
	h->wait_pa = ring_produce(&d->ep0, 0, 0, XHCI_TRB_TYPE_STATUS | XHCI_TRB_IOC | (in ? 0 : XHCI_TRB_DIR_IN));
	h->wait_done = false;
	h->wait_type = XHCI_EVT_XFER;
	h->wait_slot = d->t.slot;
	h->wait_on = true;
	doorbell(h, d->t.slot, 1);
	irq_restore(f);
	if (!wait_done(h, 1000))
		return -ETIMEDOUT;
	if (h->wait_code == XHCI_CODE_SUCCESS)
		return 0;
	/* a stall (unsupported request) or other error halts EP0: recover it
	 * so the next request works (review F6) */
	if (ep_reset(d, 1, &d->ep0) == 0)
		usb.ep0_resets++;
	return -EIO;
}

static int get_desc(struct usb_dev *d, uint8_t type, uint16_t len)
{
	return control(d, 0x80, UR_GET_DESCRIPTOR, (uint16_t)(type << 8), 0, len);
}

/* ---------------------------------------------------------------- topology */

uint32_t usb_route(const struct usb_topo *parent, uint8_t port)
{
	if (!parent)
		return 0;
	if (port == 0 || port > MAX_HUB_PORTS || parent->depth >= MAX_DEPTH)
		return 0xffffffffu; /* not addressable */
	return parent->route | (uint32_t)port << (4 * parent->depth);
}

void usb_tt(const struct usb_topo *parent, uint8_t port, uint8_t speed, uint8_t *tt_slot, uint8_t *tt_port, bool *mtt)
{
	*tt_slot = *tt_port = 0;
	*mtt = false;
	if (speed != XHCI_SPEED_FULL && speed != XHCI_SPEED_LOW)
		return;
	/* walk up to the nearest high-speed hub; `port` is where the path
	 * towards the device leaves it (OpenBSD usbd_new_device, myhsport) */
	for (const struct usb_topo *h = parent; h; port = h->port, h = h->parent)
		if (h->speed == XHCI_SPEED_HIGH) {
			*tt_slot = h->slot;
			*tt_port = port;
			*mtt = h->hub_mtt;
			return;
		}
}

/* Slot context of `d` with `last_dci` context entries (xHCI 6.2.2). */
static void slot_ctx(struct usb_dev *d, uint32_t *s, unsigned last_dci)
{
	s[0] = XHCI_SCTX_ROUTE(d->t.route) | XHCI_SCTX_SPEED(d->t.speed) | XHCI_SCTX_DCI(last_dci) |
	       XHCI_SCTX_MTT(d->hub ? d->t.hub_mtt : d->mtt) | XHCI_SCTX_HUB(d->hub);
	s[1] = XHCI_SCTX_RHPORT(d->rootport) | (d->hub ? XHCI_SCTX_NPORTS(d->hub_ports) : 0);
	s[2] = XHCI_SCTX_TT_HUB_SID(d->tt_slot) | XHCI_SCTX_TT_PORT_NUM(d->tt_port) |
	       (d->hub ? XHCI_SCTX_TT_THINK_TIME(d->hub_ttt) : 0);
	s[3] = 0;
	memcpy(d->sctx_sent, s, sizeof(d->sctx_sent)); /* for usb_info: QEMU keeps only some of it */
}

/* xHCI interval (2^n * 125 us) from bInterval (USB 2.0 9.6.6). */
static uint32_t ep_interval(uint8_t speed, uint8_t binterval)
{
	if (speed == XHCI_SPEED_HIGH || speed >= XHCI_SPEED_SUPER)
		return binterval >= 1 && binterval <= 16 ? binterval - 1u : 3;
	unsigned frames = binterval ? binterval : 1, n = 3; /* 1 ms = 2^3 * 125 us */
	while ((1u << (n + 1)) <= frames * 8u && n < 10)
		n++;
	return n;
}

/* ---------------------------------------------------------------- attach and detach */

static void free_dev(struct usb_dev *d)
{
	ring_free(&d->ep0);
	for (int i = 0; i < d->neps; i++) {
		ring_free(&d->eps[i].ring);
		if (d->eps[i].buf)
			page_free(d->eps[i].buf, 0);
	}
	if (d->ctx)
		page_free(d->ctx, 0);
	kfree(d);
}

static void detach(struct usb_dev *d)
{
	struct xhci *h = d->hc;
	for (int p = 1; p <= MAX_HUB_PORTS; p++)
		if (d->child[p])
			detach(d->child[p]);
	bool disabled = command(h, 0, 0, XHCI_CMD_DISABLE_SLOT | XHCI_TRB_SET_SLOT(d->t.slot), NULL) == 0;
	if (disabled && test_fail_disable > 0) { /* ktest 089 */
		test_fail_disable--;
		disabled = false;
	}
	uint64_t f = irq_save(); /* the interrupt handler looks slots up */
	h->slots[d->t.slot] = NULL;
	h->dcbaa[d->t.slot] = 0;
	if (d->parent)
		d->parent->child[d->t.port] = NULL;
	else
		h->root[d->t.port] = NULL;
	irq_restore(f);
	for (int i = 0; i < d->neps; i++) {
		struct usb_ep *ep = &d->eps[i];
		if (ep->kind == EP_KBD) {
			kbd_ids &= ~(1u << ep->kbd_id);
			usb.keyboards--;
		} else if (ep->kind == EP_MOUSE) {
			usb.mice--;
		}
	}
	if (d->hub)
		usb.hubs--;
	usb.devices--;
	usb.detached++;
	kprintf("usb: c%d p%s %04x:%04x detached\n", (int)(h->st - xhci_hc), d->path, d->desc.vendor, d->desc.product);
	if (disabled) {
		free_dev(d);
		return;
	}
	/* If Disable Slot failed the controller may still use the pages, so
	 * they are kept, not freed. Counted, and after 3 failures the
	 * controller attaches nothing more (review 2b L4): a flapping device
	 * cannot leak memory without bound. shortcut: no controller reset
	 * (HCRST) to reclaim them; the pages come back at reboot. */
	unsigned pages = 2 + 2u * (unsigned)d->neps;
	usb.leaked_pages += pages;
	usb.disable_failures++;
	if (++h->disable_failures >= 3 && !h->attach_off) {
		h->attach_off = true;
		kprintf("xhci: c%d: Disable Slot failed 3 times (%lu pages kept); attaching nothing more on this "
			"controller until reboot\n", (int)(h->st - xhci_hc), usb.leaked_pages);
	}
}

static struct usb_ep *add_ep(struct usb_dev *d, uint8_t kind, const struct rs_usb_iface *i)
{
	unsigned dci = (i->ep_addr & 0x0f) * 2 + 1;
	if (d->neps >= MAX_EPS || dci < 3 || dci > 31 || i->ep_max_packet == 0)
		return NULL;
	for (int k = 0; k < d->neps; k++)
		if (d->eps[k].dci == dci)
			return NULL; /* two interfaces claiming one endpoint: device data is wrong */
	struct usb_ep *ep = &d->eps[d->neps];
	memset(ep, 0, sizeof(*ep));
	ep->buf = dma_page(NULL);
	if (!ep->buf || !ring_init(&ep->ring)) {
		if (ep->buf)
			page_free(ep->buf, 0);
		ep->buf = NULL;
		return NULL;
	}
	ep->kind = kind;
	ep->dci = (uint8_t)dci;
	ep->addr = i->ep_addr;
	ep->mps = i->ep_max_packet;
	ep->ival = (uint8_t)ep_interval(d->t.speed, i->ep_interval);
	ep->kbd_id = -1;
	d->neps++;
	return ep;
}

/* Configure Endpoint adding class endpoints first..neps-1 of `d`, with the
 * slot context (it carries the hub fields). Endpoints already running are
 * not added again: that would restart them at the start of their ring. */
static int configure_eps(struct usb_dev *d, int first)
{
	struct xhci *h = d->hc;
	unsigned last = 1;
	uint32_t add = XHCI_INCTX_MASK_DCI(0);
	memset(h->in_ctx, 0, PAGE_SIZE);
	for (int i = 0; i < d->neps; i++)
		last = MAX(last, d->eps[i].dci);
	for (int i = first; i < d->neps; i++) {
		struct usb_ep *ep = &d->eps[i];
		uint32_t *c = ctx(h, h->in_ctx, ep->dci + 1u);
		c[0] = XHCI_EPCTX_SET_IVAL(ep->ival);
		c[1] = XHCI_EPCTX_SET_CERR(3) | XHCI_EPCTX_SET_EPTYPE(7) | XHCI_EPCTX_SET_MPS(ep->mps);
		*(uint64_t *)&c[2] = ep->ring.pa | 1;
		c[4] = XHCI_EPCTX_AVG_TRB_LEN(ep->mps) | XHCI_EPCTX_MAX_ESIT_PAYLOAD(ep->mps);
		add |= XHCI_INCTX_MASK_DCI(ep->dci);
	}
	ctx(h, h->in_ctx, 0)[1] = add;
	slot_ctx(d, ctx(h, h->in_ctx, 1), last);
	return command(h, virt_to_phys(h->in_ctx), 0, XHCI_CMD_CONFIG_EP | XHCI_TRB_SET_SLOT(d->t.slot), NULL);
}

static void start_eps(struct usb_dev *d)
{
	uint64_t f = irq_save();
	for (int i = 0; i < d->neps; i++)
		ep_queue(d, &d->eps[i]);
	irq_restore(f);
}

static void hub_explore(struct usb_dev *hub, uint32_t ports);

/* Hub set-up (OpenBSD uhub_attach order). */
static bool hub_attach(struct usb_dev *d, const struct rs_usb_iface *hi)
{
	if (d->t.depth >= MAX_DEPTH) {
		kprintf("usb: p%s: hub too deep, ignored\n", d->path);
		return false;
	}
	d->ss_hub = d->t.speed >= XHCI_SPEED_SUPER; /* PSIV 4 Gen 1, 5 Gen 2, ... */
	if (d->ss_hub && control(d, 0x20, UR_SET_HUB_DEPTH, d->t.depth, 0, 0) != 0)
		return false; /* its downstream port nibble in the route string */
	if (control(d, 0xa0, UR_GET_DESCRIPTOR, d->ss_hub ? 0x2a00 : 0x2900, 0, d->ss_hub ? 12 : 71) != 0)
		return false;
	int64_t hd = rs_usb_hub_desc(d->hc->dma, d->ss_hub ? 12 : 71, d->ss_hub);
	if (hd < 0)
		return false;
	unsigned nports = (unsigned)(hd & 0xff);
	if (nports > MAX_HUB_PORTS)
		kprintf("usb: p%s: hub has %u ports, using the first %u\n", d->path, nports, MAX_HUB_PORTS);
	d->hub = true;
	d->hub_ports = (uint8_t)MIN(nports, (unsigned)MAX_HUB_PORTS);
	d->hub_ttt = (uint8_t)((hd >> 8) & 3);
	d->power_on_ms = (uint16_t)(hd >> 16);
	/* a multi-TT hub runs one TT per port only after SET_INTERFACE to
	 * its multi-TT alternate setting (USB 2.0 11.23.1) */
	if (d->t.speed == XHCI_SPEED_HIGH && d->desc.protocol == 2 && hi->extra != 0xff &&
	    control(d, 0x01, UR_SET_INTERFACE, hi->extra, hi->interface, 0) == 0)
		d->t.hub_mtt = true;
	if (!add_ep(d, EP_HUB, hi) || configure_eps(d, 0) != 0)
		return false;
	for (unsigned p = 1; p <= d->hub_ports; p++)
		control(d, 0x23, 3 /* SET_FEATURE */, UHF_PORT_POWER, (uint16_t)p, 0);
	delay_ms(d->power_on_ms + 20u);
	usb.hubs++;
	start_eps(d);
	kprintf("usb: c%d p%s hub, %u ports%s%s\n", (int)(d->hc->st - xhci_hc), d->path, d->hub_ports,
		d->t.speed == XHCI_SPEED_HIGH ? (d->t.hub_mtt ? ", multi-TT" : ", single TT") : "",
		d->ss_hub ? ", SuperSpeed" : "");
	hub_explore(d, 0xfffe); /* look at every port once */
	return true;
}

static void kbd_attach(struct usb_dev *d, const struct rs_usb_iface *ki)
{
	int id = -1;
	for (int i = 0; i < USB_MAX_KBDS; i++)
		if (!(kbd_ids & (1u << i))) {
			id = i;
			break;
		}
	if (id < 0) {
		kprintf("usb: p%s: too many keyboards\n", d->path);
		return;
	}
	/* SET_PROTOCOL(boot): boot keyboards default to it anyway; may stall */
	control(d, 0x21, 0x0b, 0, ki->interface, 0);
	struct usb_ep *ep = add_ep(d, EP_KBD, ki);
	if (!ep)
		return;
	ep->kbd_id = id;
	kbd_ids |= 1u << id;
	rs_usb_kbd_reset((uint32_t)id);
	usb.keyboards++;
}

static void mouse_attach(struct usb_dev *d, const struct rs_usb_iface *mi)
{
	control(d, 0x21, 0x0b, 0, mi->interface, 0); /* SET_PROTOCOL(boot) */
	if (add_ep(d, EP_MOUSE, mi))
		usb.mice++;
}

/* Address a new device on root port `rootport` (parent NULL) or on `port`
 * of hub `parent`, read its descriptors and start the class driver. */
static struct usb_dev *attach(struct xhci *h, struct usb_dev *parent, uint8_t port, uint8_t speed)
{
	uint32_t route = usb_route(parent ? &parent->t : NULL, port);
	if (route == 0xffffffffu)
		return NULL;
	if (h->attach_off) {
		usb.attach_refused++;
		return NULL;
	}
	uint8_t slot = 0;
	if (command(h, 0, 0, XHCI_CMD_ENABLE_SLOT, &slot) != 0)
		return NULL;
	if (slot == 0 || slot > MIN(h->st->nslots, MAX_SLOTS)) {
		if (slot) /* not ours to use, but enabled: give it back */
			command(h, 0, 0, XHCI_CMD_DISABLE_SLOT | XHCI_TRB_SET_SLOT(slot), NULL);
		return NULL;
	}
	struct usb_dev *d = kzalloc(sizeof(*d));
	if (!d || !(d->ctx = dma_page(&d->ctx_pa)) || !ring_init(&d->ep0)) {
		if (d)
			free_dev(d);
		command(h, 0, 0, XHCI_CMD_DISABLE_SLOT | XHCI_TRB_SET_SLOT(slot), NULL);
		return NULL;
	}
	d->hc = h;
	d->parent = parent;
	d->t.parent = parent ? &parent->t : NULL;
	d->t.slot = slot;
	d->t.speed = speed;
	d->t.port = port;
	d->t.depth = parent ? parent->t.depth + 1 : 0;
	d->t.route = route;
	d->rootport = parent ? parent->rootport : port;
	if (parent)
		ksnprintf(d->path, sizeof(d->path), "%s.%u", parent->path, port);
	else
		ksnprintf(d->path, sizeof(d->path), "%u", port);
	usb_tt(d->t.parent, port, speed, &d->tt_slot, &d->tt_port, &d->mtt);
	uint64_t f = irq_save();
	h->slots[slot] = d;
	h->dcbaa[slot] = d->ctx_pa;
	if (parent)
		parent->child[port] = d;
	else
		h->root[port] = d;
	irq_restore(f);
	usb.devices++;
	usb.attached++;

	/* Address Device with EP0 (xHCI 4.3.3) */
	uint32_t mps = speed >= XHCI_SPEED_SUPER ? 512 : speed == XHCI_SPEED_HIGH ? 64 : 8;
	memset(h->in_ctx, 0, PAGE_SIZE);
	ctx(h, h->in_ctx, 0)[1] = XHCI_INCTX_MASK_DCI(0) | XHCI_INCTX_MASK_DCI(1);
	slot_ctx(d, ctx(h, h->in_ctx, 1), 1);
	uint32_t *ep0 = ctx(h, h->in_ctx, 2);
	ep0[1] = XHCI_EPCTX_SET_CERR(3) | XHCI_EPCTX_SET_EPTYPE(4) | XHCI_EPCTX_SET_MPS(mps);
	*(uint64_t *)&ep0[2] = d->ep0.pa | 1; /* dequeue pointer, DCS = 1 */
	ep0[4] = XHCI_EPCTX_AVG_TRB_LEN(8);
	if (command(h, virt_to_phys(h->in_ctx), 0, XHCI_CMD_ADDRESS_DEVICE | XHCI_TRB_SET_SLOT(slot), NULL) != 0)
		goto unused;
	/* the first 8 bytes of the device descriptor give EP0's real max packet size */
	if (get_desc(d, 1, 8) != 0)
		goto unused;
	int m = rs_usb_max_packet0(h->dma, 8);
	if (m < 0)
		goto unused;
	uint32_t real = m == 9 ? 512 : (uint32_t)m;
	if (real != mps) { /* Evaluate Context with the new EP0 size */
		memset(h->in_ctx, 0, PAGE_SIZE);
		ctx(h, h->in_ctx, 0)[1] = XHCI_INCTX_MASK_DCI(1);
		ctx(h, h->in_ctx, 2)[1] = XHCI_EPCTX_SET_CERR(3) | XHCI_EPCTX_SET_EPTYPE(4) | XHCI_EPCTX_SET_MPS(real);
		if (command(h, virt_to_phys(h->in_ctx), 0, XHCI_CMD_EVAL_CTX | XHCI_TRB_SET_SLOT(slot), NULL) != 0)
			goto unused;
	}
	if (get_desc(d, 1, 18) != 0 || rs_usb_device_desc(h->dma, 18, &d->desc) < 0)
		goto unused;
	d->answered = true;
	if (get_desc(d, 2, 9) != 0)
		goto unused;
	int total = rs_usb_config_total(h->dma, 9);
	if (total < 9 || get_desc(d, 2, (uint16_t)MIN(total, (int)PAGE_SIZE)) != 0)
		goto unused;
	total = MIN(total, (int)PAGE_SIZE); /* composite devices: the whole configuration */

	struct rs_usb_iface hi, ki, mi;
	bool is_hub = rs_usb_find_hub(h->dma, (size_t)total, &hi) == 0;
	bool is_kbd = !is_hub && rs_usb_find_boot_kbd(h->dma, (size_t)total, &ki) == 0;
	bool is_mouse = !is_hub && rs_usb_find_boot_mouse(h->dma, (size_t)total, &mi) == 0;
	const char *kind = is_hub ? "hub" : is_kbd && is_mouse ? "keyboard+mouse" : is_kbd ? "keyboard" : is_mouse ? "mouse" : "no driver";
	kprintf("usb: c%d p%s %s %04x:%04x class %02x: %s\n", (int)(h->st - xhci_hc), d->path,
		usb_speed_name(speed), d->desc.vendor, d->desc.product, d->desc.class, kind);
	if (!is_hub && !is_kbd && !is_mouse)
		return d; /* shortcut: stays addressed but unused (no class driver) */
	uint8_t cfg = is_hub ? hi.config_value : is_kbd ? ki.config_value : mi.config_value;
	if (control(d, 0x00, 9 /* SET_CONFIGURATION */, cfg, 0, 0) != 0)
		return d;
	if (is_hub) {
		hub_attach(d, &hi);
		return d;
	}
	if (is_kbd)
		kbd_attach(d, &ki);
	if (is_mouse)
		mouse_attach(d, &mi);
	if (d->neps == 0 || configure_eps(d, 0) != 0)
		return d;
	if (is_kbd) /* SET_IDLE(0): report on change only; may stall */
		control(d, 0x21, 0x0a, 0, ki.interface, 0);
	start_eps(d);
	return d;
unused:
	kprintf("usb: c%d p%s: device does not answer, port left alone\n", (int)(h->st - xhci_hc), d->path);
	return d;
}

/* ---------------------------------------------------------------- hub ports (OpenBSD uhub_explore) */

static int hub_feature(struct usb_dev *hub, bool set, uint16_t feature, unsigned port)
{
	return control(hub, 0x23, set ? 3 : 1, feature, (uint16_t)port, 0);
}

static int64_t hub_port_status(struct usb_dev *hub, unsigned port)
{
	if (control(hub, 0xa3, 0 /* GET_STATUS */, 0, (uint16_t)port, 4) != 0)
		return -1;
	return rs_usb_port_status(hub->hc->dma, 4);
}

static void hub_port_connect(struct usb_dev *hub, unsigned port)
{
	delay_ms(100); /* power-up and debounce (USB 2.0 7.1.7.3) */
	if (hub_feature(hub, true, UHF_PORT_RESET, port) != 0)
		return;
	int64_t ps = -1;
	for (int i = 0; i < 50; i++) { /* reset done: RESET clear, C_PORT_RESET set */
		delay_ms(10);
		ps = hub_port_status(hub, port);
		if (ps >= 0 && !(ps & UPS_RESET) && ((ps >> 16) & UPS_C_RESET))
			break;
	}
	if (ps < 0)
		return;
	hub_feature(hub, false, UHF_C_PORT_RESET, port);
	delay_ms(10); /* reset recovery */
	ps = hub_port_status(hub, port);
	if (ps < 0 || !(ps & UPS_CONNECT))
		return; /* gone after the reset */
	uint8_t speed = hub->ss_hub ? hub->t.speed
		      : (ps & UPS_HIGH_SPEED) ? XHCI_SPEED_HIGH
		      : (ps & UPS_LOW_SPEED) ? XHCI_SPEED_LOW : XHCI_SPEED_FULL;
	if (hub->t.speed == XHCI_SPEED_FULL && speed == XHCI_SPEED_HIGH)
		speed = XHCI_SPEED_FULL; /* a full-speed hub cannot run high speed */
	attach(hub->hc, hub, (uint8_t)port, speed);
}

static void hub_explore(struct usb_dev *hub, uint32_t ports)
{
	for (unsigned p = 1; p <= hub->hub_ports && hub->hub; p++) {
		if (!(ports & (1u << p)))
			continue;
		int64_t ps = hub_port_status(hub, p);
		if (ps < 0)
			continue;
		uint16_t status = (uint16_t)ps, change = (uint16_t)(ps >> 16);
		if (change & UPS_C_ENABLE)
			hub_feature(hub, false, UHF_C_PORT_ENABLE, p);
		if (change & UPS_C_OVERCURRENT)
			hub_feature(hub, false, UHF_C_PORT_OVER_CURRENT, p);
		if (change & UPS_C_RESET) {
			hub_feature(hub, false, UHF_C_PORT_RESET, p);
			change |= UPS_C_CONNECT;
		}
		if (hub->ss_hub && (change & UPS_C_BH_RESET))
			hub_feature(hub, false, UHF_C_BH_PORT_RESET, p);
		if (hub->ss_hub && (change & UPS_C_LINK_STATE))
			hub_feature(hub, false, UHF_C_PORT_LINK_STATE, p);
		bool connected = status & UPS_CONNECT;
		if (change & UPS_C_CONNECT) {
			hub_feature(hub, false, UHF_C_PORT_CONNECTION, p);
			/* a connect change with a device attached means it left,
			 * even if a new one is already there (OpenBSD rule) */
			if (hub->child[p])
				detach(hub->child[p]);
		}
		if (connected && !hub->child[p]) {
			if (!usb_connect_allowed(&hub->port_rate[p], uptime_ms())) {
				hub->hub_deferred |= 1u << p;
				rate_deferred(&hub->port_rate[p], (int)(hub->hc->st - xhci_hc), hub->path, p);
				continue;
			}
			hub_port_connect(hub, p);
			if (!hub->child[p] || !hub->child[p]->answered)
				usb_connect_failed(&hub->port_rate[p]);
		}
		else if (!connected && hub->child[p])
			detach(hub->child[p]);
	}
}

/* ---------------------------------------------------------------- root ports */

static bool port_reset(struct xhci *h, unsigned port, uint32_t *sc)
{
	uint32_t v = oread32(h, XHCI_PORTSC(port));
	if (!(v & XHCI_PS_CCS))
		return false;
	if (!(v & XHCI_PS_PED)) { /* USB 2 ports need a reset; USB 3 ports enable themselves */
		owrite32(h, XHCI_PORTSC(port), (v & ~XHCI_PS_CLEAR) | XHCI_PS_PR);
		if (!wait_until((volatile uint32_t *)(h->op + XHCI_PORTSC(port)), XHCI_PS_PRC, XHCI_PS_PRC, 200))
			return false;
		v = oread32(h, XHCI_PORTSC(port));
	}
	owrite32(h, XHCI_PORTSC(port), (v & ~XHCI_PS_CLEAR) | (v & (XHCI_PS_CSC | XHCI_PS_PRC | XHCI_PS_PEC | XHCI_PS_WRC)));
	*sc = oread32(h, XHCI_PORTSC(port));
	return (*sc & XHCI_PS_PED) != 0;
}

/* A root port with a change (or, at boot, every port). */
static void root_explore(struct xhci *h, unsigned port, bool boot)
{
	uint32_t sc = oread32(h, XHCI_PORTSC(port));
	uint32_t chg = sc & (XHCI_PS_CSC | XHCI_PS_PEC | XHCI_PS_WRC | XHCI_PS_OCC | XHCI_PS_PRC | XHCI_PS_PLC | XHCI_PS_CEC);
	if (chg)
		owrite32(h, XHCI_PORTSC(port), (sc & ~XHCI_PS_CLEAR) | chg); /* RW1C */
	struct usb_dev *d = h->root[port];
	if ((chg & XHCI_PS_CSC) && d) { /* left, or left and came back */
		detach(d);
		d = NULL;
	}
	if (!(sc & XHCI_PS_CCS)) {
		if (d)
			detach(d);
		return;
	}
	if (d)
		return;
	struct usb_rate *rate = &h->root_rate[port];
	if (!usb_connect_allowed(rate, uptime_ms())) {
		h->deferred[port / 64] |= 1ULL << (port % 64);
		rate_deferred(rate, (int)(h->st - xhci_hc), "", port);
		return;
	}
	if (!boot)
		delay_ms(100); /* debounce a new connection (USB 2.0 7.1.7.3) */
	uint32_t ps;
	d = port_reset(h, port, &ps) ? attach(h, NULL, (uint8_t)port, (uint8_t)XHCI_PS_SPEED(ps)) : NULL;
	if (!d || !d->answered)
		usb_connect_failed(rate);
}

/* ---------------------------------------------------------------- the usb thread */

static void recover_eps(struct usb_dev *d)
{
	for (int i = 0; i < d->neps; i++) {
		struct usb_ep *ep = &d->eps[i];
		if (!ep->halted || ep->dead)
			continue;
		bool ok = ep_reset(d, ep->dci, &ep->ring) == 0;
		if (ok)
			usb.ep_resets++;
		ep->resets++;
		if (ep->code == XHCI_CODE_STALL) /* the device halted it too: CLEAR_FEATURE(ENDPOINT_HALT) */
			control(d, 0x02, 1, 0, ep->addr, 0);
		if (!ok || ep->fails >= EP_MAX_FAILS) {
			ep->dead = true;
			usb.ep_dead++;
			kprintf("usb: p%s endpoint 0x%02x: %u errors in a row (code %u), given up\n", d->path, ep->addr,
				ep->fails, ep->code);
			continue;
		}
		uint64_t f = irq_save();
		ep->halted = false;
		ep_queue(d, ep);
		irq_restore(f);
	}
}

static void service(struct xhci *h)
{
	uint64_t now = uptime_ms();
	for (unsigned w = 0; w < 4; w++) /* rate-limited ports whose window opened again */
		for (unsigned b = 0; h->deferred[w] && b < 64; b++)
			if ((h->deferred[w] & (1ULL << b)) && usb_connect_open(&h->root_rate[w * 64 + b], now)) {
				h->deferred[w] &= ~(1ULL << b);
				uint64_t f = irq_save();
				h->port_change[w] |= 1ULL << b;
				irq_restore(f);
			}
	for (unsigned w = 0; w < 4; w++) {
		uint64_t f = irq_save();
		uint64_t bits = h->port_change[w];
		h->port_change[w] = 0;
		irq_restore(f);
		for (unsigned b = 0; bits && b < 64; b++)
			if (bits & (1ULL << b)) {
				unsigned port = w * 64 + b;
				if (port >= 1 && port <= h->st->nports)
					root_explore(h, port, false);
			}
	}
	for (unsigned s = 1; s <= MAX_SLOTS; s++) {
		struct usb_dev *d = h->slots[s];
		if (!d)
			continue;
		recover_eps(d);
		for (unsigned p = 1; d->hub_deferred && p <= MAX_HUB_PORTS; p++)
			if ((d->hub_deferred & (1u << p)) && usb_connect_open(&d->port_rate[p], now)) {
				d->hub_deferred &= ~(1u << p);
				uint64_t f = irq_save();
				d->hub_change |= 1u << p;
				irq_restore(f);
			}
		if (d->hub && d->hub_change) {
			uint64_t f = irq_save();
			uint32_t bits = d->hub_change;
			d->hub_change = 0;
			irq_restore(f);
			hub_explore(d, bits);
		}
	}
}

static void usb_thread(void *arg)
{
	(void)arg;
	for (int i = 0; i < xhci_count; i++) {
		struct xhci *h = &hcs[i];
		for (unsigned p = 1; p <= h->st->nports; p++)
			root_explore(h, p, true);
	}
	usb.boot_done = true;
	bool polled = false;
	for (int i = 0; i < xhci_count; i++)
		polled |= !xhci_hc[i].vector;
	for (;;) {
		sleep_prepare(polled ? 8 : 1000);
		if (usb_work || call_fn) {
			sleep_cancel();
		} else {
			sleep_commit();
		}
		usb_work = false;
		for (int i = 0; i < xhci_count; i++) {
			struct xhci *h = &hcs[i];
			if (!h->st->vector) { /* no MSI: poll */
				uint64_t f = irq_save();
				process_events(h, IRQ_BATCH);
				irq_restore(f);
			}
			service(h);
		}
		void (*fn)(void *) = call_fn;
		if (fn) {
			call_fn = NULL;
			fn(call_arg);
			call_done = true;
		}
	}
}

int usb_call(void (*fn)(void *), void *arg)
{
	if (usb_tid < 0)
		return -ENOENT;
	call_done = false;
	call_arg = arg;
	call_fn = fn;
	usb_kick();
	for (int i = 0; i < 10000 && !call_done; i++)
		thread_sleep_ms(1);
	return call_done ? 0 : -ETIMEDOUT;
}

/* ---------------------------------------------------------------- bring-up */

/* BIOS hand-off through the USB legacy support capability (OpenBSD
 * xhci_pci_takecontroller). */
static void take_controller(struct xhci *h, uint64_t bar_size)
{
	uint32_t hcc = cread32(h, XHCI_HCCPARAMS);
	uint32_t off = XHCI_HCC_XECP(hcc) << 2;
	for (int n = 0; off && off + 4 <= bar_size && n < 64; n++) { /* the list is device data */
		uint32_t eec = cread32(h, off);
		if (eec == 0xffffffffu)
			return;
		if (XHCI_XECP_ID(eec) == XHCI_ID_USB_LEGACY && h->cap[off + XHCI_XECP_BIOS_SEM]) {
			h->cap[off + XHCI_XECP_OS_SEM] = 1;
			for (int i = 0; i < 5000 && h->cap[off + XHCI_XECP_BIOS_SEM]; i++)
				delay_ms(1);
		}
		if (!XHCI_XECP_NEXT(eec))
			return;
		off += XHCI_XECP_NEXT(eec) << 2;
	}
}

/* Intel PCHs can route USB 2/3 ports to other controllers (OpenBSD
 * xhci_pci_port_route): give every switchable port to the xHCI. */
static void intel_port_route(struct pci_dev *p)
{
	if (p->vendor != 0x8086)
		return;
	pci_write32(p, PCI_XHCI_INTEL_USB3_PSSEN, pci_read32(p, PCI_XHCI_INTEL_USB3PRM));
	pci_write32(p, PCI_XHCI_INTEL_XUSB2PR, pci_read32(p, PCI_XHCI_INTEL_XUSB2PRM));
}

static bool xhci_start(struct xhci *h)
{
	struct xhci_stats *st = h->st;
	const struct pci_bar *b = &h->pdev->bar[0];
	if (b->io || !b->base || b->size < 0x1000)
		return false;
	h->cap = vmm_map_mmio(b->base, b->size);
	if (!h->cap)
		return false;
	pci_write16(h->pdev, PCI_COMMAND, pci_read16(h->pdev, PCI_COMMAND) | PCI_CMD_MEM | PCI_CMD_MASTER);
	uint32_t caplen = h->cap[XHCI_CAPLENGTH], dboff = cread32(h, XHCI_DBOFF) & ~3u,
		 rtsoff = cread32(h, XHCI_RTSOFF) & ~0x1fu;
	/* every register block we will touch must lie inside the BAR */
	if (caplen < 0x20 || dboff + 4 * 256 > b->size || rtsoff + 0x40 > b->size ||
	    caplen + XHCI_PORTSC(255) + 4 > b->size)
		return false;
	h->op = h->cap + caplen;
	h->rt = h->cap + rtsoff;
	h->db = (volatile uint32_t *)(h->cap + dboff);
	uint32_t hcs1 = cread32(h, XHCI_HCSPARAMS1), hcs2 = cread32(h, XHCI_HCSPARAMS2), hcc = cread32(h, XHCI_HCCPARAMS);
	st->nports = (uint8_t)XHCI_HCS1_N_PORTS(hcs1);
	st->nslots = (uint8_t)MIN(XHCI_HCS1_DEVSLOT_MAX(hcs1), (uint32_t)MAX_SLOTS);
	st->csz = XHCI_HCC_CSZ(hcc) ? 64 : 32;
	if (!XHCI_HCC_AC64(hcc)) {
		kprintf("xhci: controller is not 64-bit capable, not supported\n"); /* shortcut: no <4 GiB DMA zone */
		return false;
	}
	take_controller(h, b->size);
	intel_port_route(h->pdev);

	/* reset (OpenBSD xhci_reset) */
	owrite32(h, XHCI_USBCMD, 0);
	if (!wait_until((volatile uint32_t *)(h->op + XHCI_USBSTS), XHCI_STS_HCH, XHCI_STS_HCH, 100))
		return false;
	owrite32(h, XHCI_USBCMD, XHCI_CMD_HCRST);
	if (!wait_until((volatile uint32_t *)(h->op + XHCI_USBCMD), XHCI_CMD_HCRST, 0, 1000) ||
	    !wait_until((volatile uint32_t *)(h->op + XHCI_USBSTS), XHCI_STS_CNR, 0, 1000))
		return false;

	/* data structures (OpenBSD xhci_init) */
	uint64_t dcbaa_pa = 0, erst_pa = 0;
	h->dcbaa = dma_page(&dcbaa_pa);
	h->erst = dma_page(&erst_pa);
	h->in_ctx = dma_page(NULL);
	h->dma = dma_page(NULL);
	h->root = kzalloc(((size_t)st->nports + 1) * sizeof(h->root[0]));
	h->root_rate = kzalloc(((size_t)st->nports + 1) * sizeof(h->root_rate[0]));
	if (!h->dcbaa || !h->erst || !h->in_ctx || !h->dma || !h->root || !h->root_rate || !ring_init(&h->cmd) || !(h->evt.trbs = dma_page(&h->evt.pa)))
		return false;
	h->evt.index = 0;
	h->evt.toggle = XHCI_TRB_CYCLE;
	unsigned nsp = XHCI_HCS2_SPB_MAX(hcs2);
	if (nsp) { /* scratchpad buffers the controller may use as it likes */
		uint64_t arr_pa, *arr = dma_page(&arr_pa);
		if (!arr || nsp > PAGE_SIZE / 8)
			return false;
		for (unsigned i = 0; i < nsp; i++) {
			uint64_t pa;
			if (!dma_page(&pa))
				return false;
			arr[i] = pa;
		}
		h->dcbaa[0] = arr_pa;
	}
	*(volatile uint64_t *)&h->erst[0] = h->evt.pa;
	h->erst[2] = NTRB;

	/* registers (OpenBSD xhci_config) */
	owrite32(h, XHCI_CONFIG, (oread32(h, XHCI_CONFIG) & ~XHCI_CONFIG_SLOTS_MASK) | st->nslots);
	owrite32(h, XHCI_DCBAAP_LO, (uint32_t)dcbaa_pa);
	owrite32(h, XHCI_DCBAAP_HI, (uint32_t)(dcbaa_pa >> 32));
	owrite32(h, XHCI_CRCR_LO, (uint32_t)h->cmd.pa | XHCI_CRCR_LO_RCS);
	owrite32(h, XHCI_CRCR_HI, (uint32_t)(h->cmd.pa >> 32));
	rwrite32(h, XHCI_ERSTSZ(0), XHCI_ERSTS_SET(1));
	rwrite32(h, XHCI_ERSTBA_LO(0), (uint32_t)erst_pa);
	rwrite32(h, XHCI_ERSTBA_HI(0), (uint32_t)(erst_pa >> 32));
	rwrite32(h, XHCI_ERDP_LO(0), (uint32_t)h->evt.pa);
	rwrite32(h, XHCI_ERDP_HI(0), (uint32_t)(h->evt.pa >> 32));
	rwrite32(h, XHCI_IMOD(0), XHCI_IMOD_DEFAULT);
	owrite32(h, XHCI_USBCMD, XHCI_CMD_RS);
	if (!wait_until((volatile uint32_t *)(h->op + XHCI_USBSTS), XHCI_STS_HCH, 0, 100))
		return false;

	bool msix = false;
	int vec = pci_enable_msi(h->pdev, xhci_irq, &msix);
	if (vec > 0) {
		st->vector = vec;
		st->msix = msix;
		rwrite32(h, XHCI_IMAN(0), rread32(h, XHCI_IMAN(0)) | XHCI_IMAN_INTR_ENA);
		owrite32(h, XHCI_USBCMD, oread32(h, XHCI_USBCMD) | XHCI_CMD_INTE);
	}
	st->running = true;
	return true;
}

static void xhci_init(void)
{
	if (cmdline_has("noxhci"))
		return;
	for (int i = 0; i < USB_MAX_HC; i++) {
		struct pci_dev *p = pci_find_class(0x0c, 0x03, 0x30, i);
		if (!p)
			break;
		struct xhci *h = &hcs[xhci_count];
		struct xhci_stats *st = &xhci_hc[xhci_count];
		h->st = st;
		h->pdev = p;
		st->bus = p->bus;
		st->dev = p->dev;
		st->fn = p->fn;
		st->vendor = p->vendor;
		st->device = p->device;
		xhci_count++;
		if (!xhci_start(h)) {
			kprintf("xhci: c%d %04x:%04x could not be started\n", i, p->vendor, p->device);
			continue;
		}
		kprintf("xhci: c%d %02x:%02x.%u %04x:%04x, %u ports, %u slots, %u-byte contexts, %s\n", i, p->bus,
			p->dev, p->fn, p->vendor, p->device, st->nports, st->nslots, st->csz,
			st->vector ? (st->msix ? "MSI-X" : "MSI") : "polled");
	}
	if (xhci_count == 0)
		return;
	/* shortcut: a fixed settle time for link training on real ports
	 * before the first scan; later connections are debounced per port */
	delay_ms(cmdline_has("ktest") ? 20 : 150);
	struct thread *t = thread_create("usb", usb_thread, NULL, 0);
	if (!t)
		return;
	usb_tid = t->tid;
	thread_start(t);
	for (int i = 0; i < 20000 && !usb.boot_done; i++)
		thread_sleep_ms(1);
	kprintf("usb: %u device(s), %u keyboard(s), %u mouse/mice, %u hub(s)%s\n", usb.devices, usb.keyboards,
		usb.mice, usb.hubs, usb.boot_done ? "" : " (first scan still running)");
}

/* For ktest: queue n No Op commands behind one doorbell on controller 0;
 * their completion events arrive by interrupt (the handler counts them),
 * proving MSI/MSI-X delivery. */
int xhci_noop(unsigned n)
{
	struct xhci *h = &hcs[0];
	if (xhci_count == 0 || !h->st->running || !h->st->vector)
		return -ENOENT;
	if (n == 0 || n > 64)
		return -EINVAL; /* well inside one ring lap and the event ring */
	uint64_t f = irq_save();
	for (unsigned i = 0; i < n; i++)
		ring_produce(&h->cmd, 0, 0, XHCI_CMD_NOOP);
	doorbell(h, 0, 0);
	irq_restore(f);
	return 0;
}

/* ---------------------------------------------------------------- reporting */

static void fill_info(struct usb_info *o, struct xhci *h, struct usb_dev *d)
{
	memset(o, 0, sizeof(*o));
	o->hc = (uint8_t)(h->st - xhci_hc);
	o->slot = d->t.slot;
	o->speed = d->t.speed;
	o->rootport = d->rootport;
	o->port = d->t.port;
	o->depth = d->t.depth;
	strlcpy(o->path, d->path, sizeof(o->path));
	o->route = d->t.route;
	o->tt_slot = d->tt_slot;
	o->tt_port = d->tt_port;
	o->mtt = d->hub ? d->t.hub_mtt : d->mtt;
	o->hub = d->hub;
	o->vendor = d->desc.vendor;
	o->product = d->desc.product;
	o->class = d->desc.class;
	o->hub_ports = d->hub_ports;
	o->hub_ttt = d->hub_ttt;
	memcpy(o->sctx_sent, d->sctx_sent, sizeof(o->sctx_sent));
	for (int i = 0; i < d->neps; i++) {
		const struct usb_ep *ep = &d->eps[i];
		if (ep->kind == EP_KBD) {
			o->kbd = true;
			o->kbd_dci = ep->dci;
			o->kbd_ep = ep->addr;
			o->kbd_mps = ep->mps;
			o->kbd_reports = ep->xfers;
		} else if (ep->kind == EP_MOUSE) {
			o->mouse = true;
			o->mouse_reports = ep->xfers;
		}
		if (ep->kind == EP_KBD || ep->kind == EP_MOUSE) {
			o->ep_errors += ep->errors;
			o->ep_resets += ep->resets;
			o->ep_dead |= ep->dead;
		}
	}
}

int usb_list(struct usb_info *out, int max)
{
	int n = 0;
	uint64_t f = irq_save(); /* devices come and go in the usb thread */
	for (int i = 0; i < xhci_count; i++)
		for (unsigned s = 1; s <= MAX_SLOTS; s++)
			if (hcs[i].slots[s] && n < max)
				fill_info(&out[n++], &hcs[i], hcs[i].slots[s]);
	irq_restore(f);
	return n;
}

static struct usb_dev *dev_at(int hc, int slot)
{
	if (hc < 0 || hc >= xhci_count || slot < 1 || slot > MAX_SLOTS)
		return NULL;
	return hcs[hc].slots[slot];
}

int usb_slot_ctx(int hc, int slot, uint32_t out[4])
{
	uint64_t f = irq_save();
	struct usb_dev *d = dev_at(hc, slot);
	if (d)
		for (int i = 0; i < 4; i++)
			out[i] = ctx(d->hc, d->ctx, 0)[i];
	irq_restore(f);
	return d ? 0 : -ENOENT;
}

int usb_ep_state(int hc, int slot, int dci)
{
	uint64_t f = irq_save();
	struct usb_dev *d = dev_at(hc, slot);
	int st = d && dci >= 1 && dci <= 31 ? (int)ep_state(d, (unsigned)dci) : -ENOENT;
	irq_restore(f);
	return st;
}

void xhci_dump(void)
{
	if (xhci_count == 0) {
		kprintf("usb: no xHCI controller\n");
		return;
	}
	for (int i = 0; i < xhci_count; i++) {
		const struct xhci_stats *s = &xhci_hc[i];
		kprintf("c%d %02x:%02x.%u %04x:%04x: %s, %u ports, %s vector %d; %lu events (%lu command, %lu transfer, "
			"%lu port, %lu stale), %lu irqs\n",
			i, s->bus, s->dev, s->fn, s->vendor, s->device, s->running ? "running" : "not running", s->nports,
			s->msix ? "MSI-X" : s->vector ? "MSI" : "polled", s->vector, s->events, s->cmd_events,
			s->xfer_events, s->port_events, s->stale_events, s->irqs);
	}
	static struct usb_info l[32];
	int n = usb_list(l, 32);
	for (int i = 0; i < n; i++) {
		const struct usb_info *d = &l[i];
		kprintf("c%u p%-7s slot %-2u %s %04x:%04x route %05x", d->hc, d->path, d->slot, usb_speed_name(d->speed),
			d->vendor, d->product, d->route);
		if (d->tt_slot)
			kprintf(" tt %u/%u%s", d->tt_slot, d->tt_port, d->mtt ? " mtt" : "");
		if (d->hub)
			kprintf(" hub %u ports%s", d->hub_ports, d->mtt ? " multi-TT" : "");
		if (d->kbd)
			kprintf(" kbd: %lu reports", d->kbd_reports);
		if (d->mouse)
			kprintf(" mouse: %lu reports", d->mouse_reports);
		if (d->kbd || d->mouse)
			kprintf(", %lu errors, %lu resets%s", d->ep_errors, d->ep_resets, d->ep_dead ? ", given up" : "");
		kprintf("\n");
	}
	kprintf("usb: %u attached, %u detached; %lu keyboard reports, %lu chars; mouse %lu reports, x %ld y %ld "
		"buttons %x; %lu endpoint errors, %lu resets (%lu EP0), %u given up\n",
		usb.attached, usb.detached, usb.kbd_reports, usb.kbd_chars, usb.mouse_reports, usb.mouse_x, usb.mouse_y,
		usb.mouse_buttons, usb.ep_errors, usb.ep_resets, usb.ep0_resets, usb.ep_dead);
	kprintf("usb: %u connects deferred by the rate limit; %u Disable Slot failures, %lu pages kept, %u attaches refused\n",
		usb.connects_limited, usb.disable_failures, usb.leaked_pages, usb.attach_refused);
}

void usb_hwinfo(void)
{
	for (int i = 0; i < xhci_count; i++) {
		const struct xhci_stats *s = &xhci_hc[i];
		hwinfo_add("c%d %02x:%02x.%u %04x:%04x%s", i, s->bus, s->dev, s->fn, s->vendor, s->device,
			   s->running ? "" : " off");
	}
	static struct usb_info l[32];
	int n = usb_list(l, 32);
	for (int i = 0; i < n; i++) {
		const struct usb_info *d = &l[i];
		hwinfo_add("c%u p%s %04x:%04x %s %s", d->hc, d->path, d->vendor, d->product, usb_speed_name(d->speed),
			   d->hub ? "hub" : d->kbd && d->mouse ? "kbd+ms" : d->kbd ? "kbd" : d->mouse ? "mouse" : "-");
	}
}

/* ---------------------------------------------------------------- ktest hooks (usb thread) */

void usb_test_stall_ep0(void *arg)
{
	struct usb_test_req *r = arg;
	struct usb_dev *d = dev_at(r->hc, r->slot);
	r->result = -ENOENT;
	if (!d)
		return;
	/* a vendor request QEMU's HID device does not know: STALL */
	if (control(d, 0xc0, 0x01, 0, 0, 8) != -EIO) {
		r->result = -EINVAL;
		return;
	}
	r->result = get_desc(d, 1, 18) == 0 && rs_usb_device_desc(d->hc->dma, 18, &r->desc) == 0 ? 0 : -EIO;
}

void usb_test_stalling_ep(void *arg)
{
	struct usb_test_req *r = arg;
	struct usb_dev *d = dev_at(r->hc, r->slot);
	r->result = -ENOENT;
	if (!d)
		return;
	struct rs_usb_iface ti = { .ep_addr = 0x82, .ep_max_packet = 8, .ep_interval = 1 };
	struct usb_ep *ep = add_ep(d, EP_TEST, &ti);
	if (!ep || configure_eps(d, d->neps - 1) != 0) {
		r->result = -EIO;
		return;
	}
	uint64_t f = irq_save();
	ep_queue(d, ep);
	irq_restore(f);
	r->result = 0;
}

void usb_test_disable_failures(void *arg)
{
	struct usb_test_req *r = arg;
	struct usb_dev *d = dev_at(r->hc, r->slot);
	r->result = -ENOENT;
	r->pages = 0;
	if (!d || d->parent)
		return;
	struct xhci *h = d->hc;
	uint8_t port = d->t.port;
	int again = 0;
	for (int i = 0; i < 3 && d; i++) {
		r->pages += 2 + 2u * (unsigned)d->neps;
		test_fail_disable = 1;
		detach(d);
		h->root_rate[port] = (struct usb_rate){ 0 }; /* this test is not about the rate limit */
		root_explore(h, port, true);
		d = h->root[port];
		again += d != NULL;
	}
	r->result = again;
}

static void xhci_sysinit(void) { xhci_init(); }
SYSINIT(xhci, 50002, xhci_sysinit); /* needs threads and interrupts on */
