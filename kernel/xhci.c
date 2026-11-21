/* SPDX-License-Identifier: ISC AND BSD-2-Clause */
/*
 * Copyright (c) 2026 zenmastercat and Magia contributors
 *
 * Portions adapted from OpenBSD sys/dev/usb/xhci.c (xhci_reset,
 * xhci_init/xhci_config register sequence, xhci_ring_reset/_consume/
 * _produce, xhci_event_dequeue), OpenBSD src commit 8e1a11b4:
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
 * xhci_pci_port_route), which carries:
 *
 * Copyright (c) 2001, 2002 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson (lennart@augustsson.net).
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
 * xhci.c - USB 3 host controller (xHCI 1.2) driving one HID boot keyboard.
 *
 * Without OpenBSD's usbdi layer: during boot the driver enumerates the
 * devices on the root ports synchronously (commands and control transfers
 * wait on the event ring), finds the first HID boot keyboard and starts
 * interrupt-IN transfers on it; afterwards an MSI-X (or MSI) interrupt
 * handles the event ring, hands each report to Rust (rust/src/usb.rs) and
 * re-queues the transfer. Event TRB fields are checked (type, slot,
 * endpoint, completion code, residual length; pointers are only compared
 * with our own rings, never dereferenced); device-written data
 * (descriptors, reports) is parsed in Rust; the controller's DMA is not
 * restricted (no IOMMU, see docs/ARCHITECTURE.md).
 * The first HID boot keyboard found is trusted as the console keyboard.
 * shortcut: no hubs, no hot-plug, one keyboard, and a halted keyboard
 * endpoint is not reset (Reset Endpoint + Set TR Dequeue Pointer come in
 * stage 2b); stage 3/4 grow a usbdi-like layer for more devices.
 */
#include <magia/abi.h>
#include <magia/acpi.h>
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
#define MAX_SLOTS 32
#define REPORT_BUF 64
#define IRQ_BATCH 32 /* events handled per interrupt */

struct ring {
	volatile struct xhci_trb *trbs;
	uint64_t pa;
	unsigned index;
	uint32_t toggle;
};

struct xhci_stats xhci;
static volatile uint8_t *cap, *op, *rt;
static volatile uint32_t *db;
static struct pci_dev *pdev;
static volatile uint64_t *dcbaa;
static struct ring cmd_ring, evt_ring, kbd_ring;
static volatile uint32_t *erst;
static uint8_t *in_ctx;           /* input context, reused for every command */
static uint8_t *dma_buf;          /* control transfer data */
static uint8_t *report_buf;       /* interrupt-IN data */

SYSCTL_U64_RO(hw_usb_kbd_reports, "hw.usb.kbd_reports", &xhci.kbd_reports, "USB keyboard reports received");
SYSCTL_U64_RO(hw_usb_events, "hw.usb.events", &xhci.events, "xHCI events handled");

static uint32_t cread32(uint32_t o) { return *(volatile uint32_t *)(cap + o); }
static uint32_t oread32(uint32_t o) { return *(volatile uint32_t *)(op + o); }
static void owrite32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(op + o) = v; }
static uint32_t rread32(uint32_t o) { return *(volatile uint32_t *)(rt + o); }
static void rwrite32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(rt + o) = v; }

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

/* ---------------------------------------------------------------- rings (OpenBSD) */

static bool ring_init(struct ring *r, bool link)
{
	r->trbs = dma_page(&r->pa);
	if (!r->trbs)
		return false;
	r->index = 0;
	r->toggle = XHCI_TRB_CYCLE;
	if (link) { /* one segment: the last TRB links back to the first; ring_produce hands it over */
		volatile struct xhci_trb *t = &r->trbs[NTRB - 1];
		t->trb_paddr = r->pa;
		t->trb_flags = XHCI_TRB_TYPE_LINK | XHCI_TRB_TC;
	}
	return true;
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
 * sees a half-written TRB. Returns its physical address. */
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

static void doorbell(unsigned slot, uint32_t target)
{
	__asm__ volatile("mfence" ::: "memory");
	db[slot] = target;
}

/* ---------------------------------------------------------------- events */

struct evt {
	uint32_t type, code, slot, ep;
	uint64_t ptr;
	uint32_t len; /* residual length (transfer events) */
};

static void handle_xfer(const struct evt *e);

/* Read one event; returns false when the ring is empty. Sets ERDP. */
static bool next_event(struct evt *e)
{
	volatile struct xhci_trb *t = ring_consume(&evt_ring);
	if (!t)
		return false;
	uint32_t flags = t->trb_flags, status = t->trb_status;
	e->type = flags & XHCI_TRB_TYPE_MASK;
	e->code = XHCI_TRB_GET_CODE(status);
	e->slot = XHCI_TRB_GET_SLOT(flags);
	e->ep = XHCI_TRB_GET_EP(flags);
	e->ptr = t->trb_paddr;
	e->len = XHCI_TRB_REMAIN(status);
	uint64_t dq = evt_ring.pa + evt_ring.index * sizeof(struct xhci_trb);
	rwrite32(XHCI_ERDP_LO(0), (uint32_t)dq | XHCI_ERDP_LO_BUSY);
	rwrite32(XHCI_ERDP_HI(0), (uint32_t)(dq >> 32));
	xhci.events++;
	return true;
}

/* Wait for the event of a command or control transfer (on `slot`'s EP0;
 * slot 0 for commands); other events (port changes, keyboard reports) are
 * handled on the way. */
static bool wait_event(uint32_t type, unsigned slot, uint64_t trb_pa, struct evt *out, uint64_t ms)
{
	uint64_t end = now_ns() + ms * 1000000ULL;
	while (now_ns() < end) {
		struct evt e;
		if (!next_event(&e)) {
			cpu_relax();
			continue;
		}
		if (e.type == XHCI_EVT_CMD_COMPLETE)
			xhci.cmd_events++;
		else if (e.type == XHCI_EVT_PORT_CHANGE)
			xhci.port_events++;
		else if (e.type == XHCI_EVT_XFER)
			xhci.xfer_events++;
		bool ep0_error = type == XHCI_EVT_XFER && e.type == XHCI_EVT_XFER && e.slot == slot && e.ep == 1 &&
				 e.code != XHCI_CODE_SUCCESS && e.code != XHCI_CODE_SHORT_XFER;
		if ((e.type == type && e.ptr == trb_pa) || ep0_error) {
			*out = e;
			return true;
		}
		if (e.type == XHCI_EVT_XFER)
			handle_xfer(&e);
	}
	return false;
}

static int command(uint64_t ptr, uint32_t status, uint32_t flags, struct evt *res)
{
	uint64_t pa = ring_produce(&cmd_ring, ptr, status, flags);
	doorbell(0, 0);
	struct evt e;
	if (!wait_event(XHCI_EVT_CMD_COMPLETE, 0, pa, &e, 1000))
		return -ETIMEDOUT;
	if (res)
		*res = e;
	return e.code == XHCI_CODE_SUCCESS ? 0 : -EIO;
}

/* ---------------------------------------------------------------- contexts */

static uint32_t *ctx(uint8_t *base, unsigned idx) { return (uint32_t *)(base + idx * xhci.csz); }

/* Control transfer on EP0 of `slot`: setup, optional IN data, status. */
static int control(struct ring *ep0_ring, unsigned slot, uint8_t reqtype, uint8_t req, uint16_t value, uint16_t index, uint16_t len)
{
	if (len > PAGE_SIZE)
		return -EINVAL;
	uint64_t setup = (uint64_t)reqtype | (uint64_t)req << 8 | (uint64_t)value << 16 | (uint64_t)index << 32 |
			 (uint64_t)len << 48;
	bool in = len && (reqtype & 0x80);
	ring_produce(ep0_ring, setup, 8, XHCI_TRB_TYPE_SETUP | XHCI_TRB_IDT | (len ? (in ? XHCI_TRB_TRT_IN : XHCI_TRB_TRT_OUT) : 0));
	if (len)
		ring_produce(ep0_ring, virt_to_phys(dma_buf), len, XHCI_TRB_TYPE_DATA | (in ? XHCI_TRB_DIR_IN : 0));
	uint64_t st = ring_produce(ep0_ring, 0, 0, XHCI_TRB_TYPE_STATUS | XHCI_TRB_IOC | (in ? 0 : XHCI_TRB_DIR_IN));
	doorbell(slot, 1);
	struct evt e;
	/* success is reported on the status TRB; an error on whichever stage
	 * failed, so any error event for this slot's EP0 ends the transfer */
	if (!wait_event(XHCI_EVT_XFER, slot, st, &e, 1000))
		return -ETIMEDOUT;
	return e.code == XHCI_CODE_SUCCESS ? 0 : -EIO;
}

static uint8_t ep0_mps(uint8_t speed)
{
	return speed == XHCI_SPEED_SUPER ? 0 : speed == XHCI_SPEED_HIGH ? 64 : 8; /* 0: 512, set below */
}

/* xHCI interval (2^n * 125 us) from bInterval (USB 2.0 9.6.6). */
static uint32_t ep_interval(uint8_t speed, uint8_t binterval)
{
	if (speed == XHCI_SPEED_HIGH || speed == XHCI_SPEED_SUPER)
		return binterval >= 1 && binterval <= 16 ? binterval - 1u : 3;
	unsigned frames = binterval ? binterval : 1, n = 3; /* 1 ms = 2^3 * 125 us */
	while ((1u << (n + 1)) <= frames * 8u && n < 10)
		n++;
	return n;
}

/* ---------------------------------------------------------------- the keyboard */

static void kbd_queue(void)
{
	uint16_t len = (uint16_t)MIN(xhci.kbd_if.ep_max_packet, (uint16_t)REPORT_BUF);
	ring_produce(&kbd_ring, virt_to_phys(report_buf), len, XHCI_TRB_TYPE_NORMAL | XHCI_TRB_IOC | XHCI_TRB_ISP);
	doorbell(xhci.kbd_slot, xhci.kbd_dci);
}

static void handle_xfer(const struct evt *e)
{
	if (!xhci.kbd || e->slot != xhci.kbd_slot || e->ep != xhci.kbd_dci) {
		xhci.bad_events++;
		return;
	}
	if (e->code != XHCI_CODE_SUCCESS && e->code != XHCI_CODE_SHORT_XFER) {
		/* shortcut: the endpoint is now halted and is not reset, so the
		 * keyboard stays dead until reboot; Reset Endpoint + Set TR Dequeue
		 * Pointer and a re-queue come in stage 2b */
		xhci.kbd_errors++;
		return;
	}
	uint16_t asked = (uint16_t)MIN(xhci.kbd_if.ep_max_packet, (uint16_t)REPORT_BUF);
	uint32_t got = e->len <= asked ? asked - e->len : 0; /* residual from the device side: bound it */
	uint8_t out[16];
	size_t n = rs_usb_kbd_report(report_buf, got, out, sizeof(out));
	xhci.kbd_reports++;
	for (size_t i = 0; i < n && i < sizeof(out); i++)
		console_rx_push(out[i]);
	xhci.kbd_chars += n;
	kbd_queue();
}

static void xhci_irq(struct trapframe *tf)
{
	(void)tf;
	xhci.irqs++;
	rwrite32(XHCI_IMAN(0), rread32(XHCI_IMAN(0)) | XHCI_IMAN_INTR_PEND); /* RW1C */
	owrite32(XHCI_USBSTS, XHCI_STS_EINT);
	/* At most IRQ_BATCH events, so a device flooding the ring cannot keep
	 * the CPU here. next_event has already moved ERDP past each event and
	 * cleared EHB; with events left, the controller interrupts again (the
	 * poll thread, if used, comes back in 8 ms). */
	struct evt e;
	unsigned n = 0;
	while (n < IRQ_BATCH && next_event(&e)) {
		n++;
		if (e.type == XHCI_EVT_XFER) {
			xhci.xfer_events++;
			handle_xfer(&e);
		} else if (e.type == XHCI_EVT_PORT_CHANGE) {
			xhci.port_events++;
		} else if (e.type == XHCI_EVT_CMD_COMPLETE) {
			xhci.cmd_events++;
		}
	}
	xhci.irq_max_events = MAX(xhci.irq_max_events, n);
}

static void poll_thread(void *arg)
{
	(void)arg;
	for (;;) {
		uint64_t f = irq_save();
		xhci_irq(NULL);
		irq_restore(f);
		thread_sleep_ms(8);
	}
}

/* ---------------------------------------------------------------- enumeration */

static bool port_reset(unsigned port, uint32_t *sc)
{
	uint32_t v = oread32(XHCI_PORTSC(port));
	if (!(v & XHCI_PS_CCS))
		return false;
	if (!(v & XHCI_PS_PED)) { /* USB 2 ports need a reset; USB 3 ports enable themselves */
		owrite32(XHCI_PORTSC(port), (v & ~XHCI_PS_CLEAR) | XHCI_PS_PR);
		if (!wait_until((volatile uint32_t *)(op + XHCI_PORTSC(port)), XHCI_PS_PRC, XHCI_PS_PRC, 200))
			return false;
		v = oread32(XHCI_PORTSC(port));
	}
	owrite32(XHCI_PORTSC(port), (v & ~XHCI_PS_CLEAR) | (v & (XHCI_PS_CSC | XHCI_PS_PRC | XHCI_PS_PEC | XHCI_PS_WRC)));
	*sc = oread32(XHCI_PORTSC(port));
	return (*sc & XHCI_PS_PED) != 0;
}

/* Set up the device on `port`; keep it if it is a HID boot keyboard. */
static void attach_port(unsigned port)
{
	uint32_t sc;
	if (!port_reset(port, &sc))
		return;
	xhci.devices++;
	uint8_t speed = XHCI_PS_SPEED(sc);
	struct evt e;
	if (command(0, 0, XHCI_CMD_ENABLE_SLOT, &e) != 0 || e.slot == 0 || e.slot > xhci.nslots)
		return;
	unsigned slot = e.slot;
	uint64_t dev_pa;
	struct ring ep0_ring;
	if (!dma_page(&dev_pa) || !ring_init(&ep0_ring, true))
		return; /* shortcut: pages of devices that are not kept are not freed (boot only) */
	memset(in_ctx, 0, PAGE_SIZE);
	dcbaa[slot] = dev_pa;

	uint32_t mps = speed == XHCI_SPEED_SUPER ? 512 : ep0_mps(speed);
	ctx(in_ctx, 0)[1] = XHCI_INCTX_MASK_DCI(0) | XHCI_INCTX_MASK_DCI(1);
	uint32_t *slotc = ctx(in_ctx, 1), *ep0 = ctx(in_ctx, 2);
	slotc[0] = XHCI_SCTX_SPEED(speed) | XHCI_SCTX_DCI(1);
	slotc[1] = XHCI_SCTX_RHPORT(port);
	ep0[1] = XHCI_EPCTX_SET_CERR(3) | XHCI_EPCTX_SET_EPTYPE(4) | XHCI_EPCTX_SET_MPS(mps);
	*(uint64_t *)&ep0[2] = ep0_ring.pa | 1; /* dequeue pointer, DCS = 1 */
	ep0[4] = XHCI_EPCTX_AVG_TRB_LEN(8);
	if (command(virt_to_phys(in_ctx), 0, XHCI_CMD_ADDRESS_DEVICE | XHCI_TRB_SET_SLOT(slot), NULL) != 0)
		return;

	/* first 8 bytes of the device descriptor give EP0's real max packet size */
	if (control(&ep0_ring, slot, 0x80, 6, 0x0100, 0, 8) != 0)
		return;
	int m = rs_usb_max_packet0(dma_buf, 8);
	if (m < 0)
		return;
	uint32_t real = m == 9 ? 512 : (uint32_t)m;
	if (real != mps) { /* Evaluate Context with the new EP0 size */
		ctx(in_ctx, 0)[1] = XHCI_INCTX_MASK_DCI(1);
		ep0[1] = XHCI_EPCTX_SET_CERR(3) | XHCI_EPCTX_SET_EPTYPE(4) | XHCI_EPCTX_SET_MPS(real);
		if (command(virt_to_phys(in_ctx), 0, XHCI_CMD_EVAL_CTX | XHCI_TRB_SET_SLOT(slot), NULL) != 0)
			return;
	}
	struct rs_usb_dev dev;
	if (control(&ep0_ring, slot, 0x80, 6, 0x0100, 0, 18) != 0 || rs_usb_device_desc(dma_buf, 18, &dev) < 0)
		return;
	if (control(&ep0_ring, slot, 0x80, 6, 0x0200, 0, 9) != 0)
		return;
	int total = rs_usb_config_total(dma_buf, 9);
	if (total < 9)
		return;
	total = MIN(total, 1024); /* shortcut: big configurations are cut, enough for keyboards */
	struct rs_usb_bootkbd kb;
	if (control(&ep0_ring, slot, 0x80, 6, 0x0200, 0, (uint16_t)total) != 0 ||
	    rs_usb_find_boot_kbd(dma_buf, (size_t)total, &kb) < 0)
		goto not_kbd;
	if (xhci.kbd)
		goto not_kbd; /* one keyboard */

	/* configure: SET_CONFIGURATION, boot protocol, endpoint, no idle reports */
	if (control(&ep0_ring, slot, 0x00, 9, kb.config_value, 0, 0) != 0)
		goto not_kbd;
	control(&ep0_ring, slot, 0x21, 0x0b, 0, kb.interface, 0); /* SET_PROTOCOL(boot): boot keyboards default to it anyway */
	unsigned dci = (kb.ep_addr & 0x0f) * 2 + 1;
	if (dci < 2 || dci > 31 || !ring_init(&kbd_ring, true))
		goto not_kbd;
	memset(in_ctx, 0, PAGE_SIZE);
	ctx(in_ctx, 0)[1] = XHCI_INCTX_MASK_DCI(0) | XHCI_INCTX_MASK_DCI(dci);
	slotc[0] = XHCI_SCTX_SPEED(speed) | XHCI_SCTX_DCI(dci);
	slotc[1] = XHCI_SCTX_RHPORT(port);
	uint32_t *epc = ctx(in_ctx, dci + 1);
	epc[0] = XHCI_EPCTX_SET_IVAL(ep_interval(speed, kb.ep_interval));
	epc[1] = XHCI_EPCTX_SET_CERR(3) | XHCI_EPCTX_SET_EPTYPE(7) | XHCI_EPCTX_SET_MPS(kb.ep_max_packet);
	*(uint64_t *)&epc[2] = kbd_ring.pa | 1;
	epc[4] = XHCI_EPCTX_AVG_TRB_LEN(kb.ep_max_packet) | XHCI_EPCTX_MAX_ESIT_PAYLOAD(kb.ep_max_packet);
	if (command(virt_to_phys(in_ctx), 0, XHCI_CMD_CONFIG_EP | XHCI_TRB_SET_SLOT(slot), NULL) != 0)
		goto not_kbd;
	control(&ep0_ring, slot, 0x21, 0x0a, 0, kb.interface, 0); /* SET_IDLE(0): report on change only; may stall */

	xhci.kbd = true;
	xhci.kbd_port = (uint8_t)port;
	xhci.kbd_slot = (uint8_t)slot;
	xhci.kbd_speed = speed;
	xhci.kbd_dci = (uint8_t)dci;
	xhci.kbd_dev = dev;
	xhci.kbd_if = kb;
	kbd_queue();
	return;
not_kbd:
	return; /* shortcut: other devices stay addressed but unused (no class drivers yet) */
}

/* ---------------------------------------------------------------- bring-up */

/* BIOS hand-off through the USB legacy support capability (OpenBSD
 * xhci_pci_takecontroller). */
static void take_controller(uint64_t bar_size)
{
	uint32_t hcc = cread32(XHCI_HCCPARAMS);
	uint32_t off = XHCI_HCC_XECP(hcc) << 2;
	for (int n = 0; off && off + 4 <= bar_size && n < 64; n++) { /* the list is device data */
		uint32_t eec = cread32(off);
		if (eec == 0xffffffffu)
			return;
		if (XHCI_XECP_ID(eec) == XHCI_ID_USB_LEGACY && cap[off + XHCI_XECP_BIOS_SEM]) {
			cap[off + XHCI_XECP_OS_SEM] = 1;
			for (int i = 0; i < 5000 && cap[off + XHCI_XECP_BIOS_SEM]; i++)
				delay_ms(1);
		}
		if (!XHCI_XECP_NEXT(eec))
			return;
		off += XHCI_XECP_NEXT(eec) << 2;
	}
}

/* Intel PCHs can route USB 2/3 ports to other controllers (OpenBSD
 * xhci_pci_port_route): give every switchable port to the xHCI. */
static void intel_port_route(void)
{
	if (pdev->vendor != 0x8086)
		return;
	pci_write32(pdev, PCI_XHCI_INTEL_USB3_PSSEN, pci_read32(pdev, PCI_XHCI_INTEL_USB3PRM));
	pci_write32(pdev, PCI_XHCI_INTEL_XUSB2PR, pci_read32(pdev, PCI_XHCI_INTEL_XUSB2PRM));
}

static bool xhci_start(void)
{
	const struct pci_bar *b = &pdev->bar[0];
	if (b->io || !b->base || b->size < 0x1000)
		return false;
	cap = vmm_map_mmio(b->base, b->size);
	if (!cap)
		return false;
	pci_write16(pdev, PCI_COMMAND, pci_read16(pdev, PCI_COMMAND) | PCI_CMD_MEM | PCI_CMD_MASTER);
	uint32_t caplen = cap[XHCI_CAPLENGTH], dboff = cread32(XHCI_DBOFF) & ~3u, rtsoff = cread32(XHCI_RTSOFF) & ~0x1fu;
	/* every register block we will touch must lie inside the BAR */
	if (caplen < 0x20 || dboff + 4 * 256 > b->size || rtsoff + 0x40 > b->size ||
	    caplen + XHCI_PORTSC(255) + 4 > b->size)
		return false;
	op = cap + caplen;
	rt = cap + rtsoff;
	db = (volatile uint32_t *)(cap + dboff);
	uint32_t hcs1 = cread32(XHCI_HCSPARAMS1), hcs2 = cread32(XHCI_HCSPARAMS2), hcc = cread32(XHCI_HCCPARAMS);
	xhci.nports = (uint8_t)XHCI_HCS1_N_PORTS(hcs1);
	xhci.nslots = (uint8_t)MIN(XHCI_HCS1_DEVSLOT_MAX(hcs1), (uint32_t)MAX_SLOTS);
	xhci.csz = XHCI_HCC_CSZ(hcc) ? 64 : 32;
	if (!XHCI_HCC_AC64(hcc)) {
		kprintf("xhci: controller is not 64-bit capable, not supported\n"); /* shortcut: no <4 GiB DMA zone */
		return false;
	}
	take_controller(b->size);
	intel_port_route();

	/* reset (OpenBSD xhci_reset) */
	owrite32(XHCI_USBCMD, 0);
	if (!wait_until((volatile uint32_t *)(op + XHCI_USBSTS), XHCI_STS_HCH, XHCI_STS_HCH, 100))
		return false;
	owrite32(XHCI_USBCMD, XHCI_CMD_HCRST);
	if (!wait_until((volatile uint32_t *)(op + XHCI_USBCMD), XHCI_CMD_HCRST, 0, 1000) ||
	    !wait_until((volatile uint32_t *)(op + XHCI_USBSTS), XHCI_STS_CNR, 0, 1000))
		return false;

	/* data structures (OpenBSD xhci_init) */
	uint64_t dcbaa_pa = 0, erst_pa = 0;
	dcbaa = dma_page(&dcbaa_pa);
	erst = dma_page(&erst_pa);
	in_ctx = dma_page(NULL);
	dma_buf = dma_page(NULL);
	report_buf = dma_page(NULL);
	if (!dcbaa || !erst || !in_ctx || !dma_buf || !report_buf || !ring_init(&cmd_ring, true) ||
	    !ring_init(&evt_ring, false))
		return false;
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
		dcbaa[0] = arr_pa;
	}
	*(volatile uint64_t *)&erst[0] = evt_ring.pa;
	erst[2] = NTRB;

	/* registers (OpenBSD xhci_config) */
	owrite32(XHCI_CONFIG, (oread32(XHCI_CONFIG) & ~XHCI_CONFIG_SLOTS_MASK) | xhci.nslots);
	owrite32(XHCI_DCBAAP_LO, (uint32_t)dcbaa_pa);
	owrite32(XHCI_DCBAAP_HI, (uint32_t)(dcbaa_pa >> 32));
	owrite32(XHCI_CRCR_LO, (uint32_t)cmd_ring.pa | XHCI_CRCR_LO_RCS);
	owrite32(XHCI_CRCR_HI, (uint32_t)(cmd_ring.pa >> 32));
	rwrite32(XHCI_ERSTSZ(0), XHCI_ERSTS_SET(1));
	rwrite32(XHCI_ERSTBA_LO(0), (uint32_t)erst_pa);
	rwrite32(XHCI_ERSTBA_HI(0), (uint32_t)(erst_pa >> 32));
	rwrite32(XHCI_ERDP_LO(0), (uint32_t)evt_ring.pa);
	rwrite32(XHCI_ERDP_HI(0), (uint32_t)(evt_ring.pa >> 32));
	rwrite32(XHCI_IMOD(0), XHCI_IMOD_DEFAULT);
	owrite32(XHCI_USBCMD, XHCI_CMD_RS); /* interrupts stay off while we enumerate by polling */
	if (!wait_until((volatile uint32_t *)(op + XHCI_USBSTS), XHCI_STS_HCH, 0, 100))
		return false;
	xhci.running = true;
	return true;
}

static void xhci_init(void)
{
	pdev = pci_find_class(0x0c, 0x03, 0x30, 0);
	if (!pdev || cmdline_has("noxhci"))
		return;
	if (!xhci_start()) {
		kprintf("xhci: %04x:%04x could not be started\n", pdev->vendor, pdev->device);
		return;
	}
	/* shortcut: a fixed settle time for link training on real ports; a
	 * port-change-driven enumerator comes with hot-plug */
	delay_ms(cmdline_has("ktest") ? 20 : 150);
	for (unsigned p = 1; p <= xhci.nports; p++)
		attach_port(p);

	bool msix = false;
	int vec = pci_enable_msi(pdev, xhci_irq, &msix);
	if (vec > 0) {
		xhci.vector = vec;
		xhci.msix = msix;
		rwrite32(XHCI_IMAN(0), rread32(XHCI_IMAN(0)) | XHCI_IMAN_INTR_ENA);
		owrite32(XHCI_USBCMD, oread32(XHCI_USBCMD) | XHCI_CMD_INTE);
	} else {
		struct thread *t = thread_create("xhci-poll", poll_thread, NULL, 0);
		if (t)
			thread_start(t);
	}
	kprintf("xhci: %04x:%04x, %u ports, %u slots, %u-byte contexts, %s; %u device(s)", pdev->vendor,
		pdev->device, xhci.nports, xhci.nslots, xhci.csz,
		vec > 0 ? (msix ? "MSI-X" : "MSI") : "polled", xhci.devices);
	if (xhci.kbd)
		kprintf(", keyboard %04x:%04x on port %u", xhci.kbd_dev.vendor, xhci.kbd_dev.product, xhci.kbd_port);
	kprintf("\n");
}

/* For ktest: queue n No Op commands behind one doorbell; their completion
 * events arrive by interrupt (the handler counts them), proving MSI/MSI-X
 * delivery. */
int xhci_noop(unsigned n)
{
	if (!xhci.running || !xhci.vector)
		return -ENOENT;
	if (n == 0 || n > 64)
		return -EINVAL; /* well inside one ring lap and the event ring */
	uint64_t f = irq_save();
	for (unsigned i = 0; i < n; i++)
		ring_produce(&cmd_ring, 0, 0, XHCI_CMD_NOOP);
	doorbell(0, 0);
	irq_restore(f);
	return 0;
}

void xhci_dump(void)
{
	if (!xhci.running) {
		kprintf("usb: no xHCI controller running\n");
		return;
	}
	kprintf("xhci: %u ports, %u slots, %s vector %d; %lu events (%lu command, %lu transfer, %lu port), %lu irqs\n",
		xhci.nports, xhci.nslots, xhci.msix ? "MSI-X" : "MSI", xhci.vector, xhci.events, xhci.cmd_events,
		xhci.xfer_events, xhci.port_events, xhci.irqs);
	for (unsigned p = 1; p <= xhci.nports; p++) {
		uint32_t sc = oread32(XHCI_PORTSC(p));
		if (sc & XHCI_PS_CCS)
			kprintf("port %u: connected, speed %u%s\n", p, XHCI_PS_SPEED(sc), sc & XHCI_PS_PED ? ", enabled" : "");
	}
	if (xhci.kbd)
		kprintf("keyboard: %04x:%04x port %u slot %u ep 0x%02x, %lu reports, %lu chars, %lu errors\n",
			xhci.kbd_dev.vendor, xhci.kbd_dev.product, xhci.kbd_port, xhci.kbd_slot, xhci.kbd_if.ep_addr,
			xhci.kbd_reports, xhci.kbd_chars, xhci.kbd_errors);
}

static void xhci_sysinit(void) { xhci_init(); }
SYSINIT(xhci, 50002, xhci_sysinit); /* needs threads (poll fallback) and interrupts on */
