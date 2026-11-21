/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * t_usb.c - ktests for the xHCI driver, hubs and the USB keyboard (07x).
 * The default boot has one qemu-xhci with a usb-kbd on a root port; with
 * "kt-desktop" on the command line (tests/run-ktest.sh "desktop" run) two
 * controllers, a usb-hub with a usb-kbd behind it, a second usb-kbd and a
 * usb-mouse, like the target desktop.
 */
#include <magia/abi.h>
#include <magia/apic.h>
#include <magia/ktest.h>
#include <magia/pci.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/usb.h>
#include <magia/xhcireg.h>

#define MAXDEV 16
static struct usb_info devs[MAXDEV];

static const struct usb_info *find(const char *path, int hc)
{
	int n = usb_list(devs, MAXDEV);
	for (int i = 0; i < n; i++)
		if (devs[i].hc == hc && strcmp(devs[i].path, path) == 0)
			return &devs[i];
	return NULL;
}

static const struct usb_info *first_kbd(void)
{
	int n = usb_list(devs, MAXDEV);
	for (int i = 0; i < n; i++)
		if (devs[i].kbd)
			return &devs[i];
	return NULL;
}

/* Every controller runs on MSI-X, and a command completes through the
 * interrupt path. */
KTEST(070, xhci_controllers_run_with_msix)
{
	bool desktop = cmdline_has("kt-desktop");
	KT_EQ(xhci_count, desktop ? 2 : 1);
	for (int i = 0; i < xhci_count; i++) {
		const struct xhci_stats *s = &xhci_hc[i];
		KT_ASSERT(s->running);
		KT_EQ(s->nports, 8);
		KT_ASSERT(s->nslots >= 1);
		KT_ASSERT(s->csz == 32 || s->csz == 64);
		KT_ASSERT(s->msix);
		KT_ASSERT(s->vector >= VEC_MSI_FIRST && s->vector <= VEC_MSI_LAST);
		KT_EQ(s->bad_events, 0);
		KT_EQ(s->vendor, 0x1b36);
	}
	KT_ASSERT(xhci_count < 2 || xhci_hc[0].vector != xhci_hc[1].vector);
	KT_ASSERT(usb.boot_done);
	const struct xhci_stats *s = &xhci_hc[0];
	uint64_t irqs = s->irqs, cmds = s->cmd_events, vec = vec_counts[s->vector];
	KT_EQ(xhci_noop(1), 0);
	for (int i = 0; i < 200 && s->cmd_events == cmds; i++)
		thread_sleep_ms(1);
	KT_EQ(s->cmd_events, cmds + 1);
	KT_ASSERT(s->irqs > irqs);
	KT_ASSERT(vec_counts[s->vector] > vec);
}

KTEST(071, usb_boot_keyboard_attached)
{
	const struct usb_info *k = first_kbd();
	KT_ASSERT(k != NULL);
	if (!k)
		return;
	KT_EQ(k->vendor, 0x0627); /* QEMU's usb-kbd */
	KT_EQ(k->product, 0x0001);
	KT_EQ(k->kbd_ep, 0x81);
	KT_EQ(k->kbd_mps, 8);
	KT_EQ(k->kbd_dci, 3); /* endpoint 1 IN */
	KT_ASSERT(k->rootport >= 1 && k->rootport <= xhci_hc[k->hc].nports);
	KT_EQ(k->ep_errors, 0);
	KT_EQ(usb.keyboards, cmdline_has("kt-desktop") ? 2 : 1);
	/* the slot context the controller keeps matches what we set up */
	uint32_t sc[4];
	KT_EQ(usb_slot_ctx(k->hc, k->slot, sc), 0);
	KT_EQ(sc[0] & 0xfffff, k->route);
	KT_EQ((sc[0] >> 20) & 0xf, k->speed);
	KT_EQ((sc[1] >> 16) & 0xff, k->rootport);
	KT_EQ(XHCI_SCTX_SLOT_STATE(sc[3]), 3); /* configured */
	KT_EQ(usb_ep_state(k->hc, k->slot, k->kbd_dci), XHCI_EP_RUNNING);
}

/* The Rust parsers behind the C interface refuse malformed device data. */
KTEST(072, usb_parsers_reject_garbage)
{
	static const uint8_t devd[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 8, 0x27, 0x06, 1, 0, 0, 0, 1, 4, 11, 1 };
	struct rs_usb_dev d;
	KT_EQ(rs_usb_device_desc(devd, 18, &d), 0);
	KT_EQ(d.vendor, 0x0627);
	KT_ASSERT(rs_usb_device_desc(devd, 17, &d) < 0);
	KT_ASSERT(rs_usb_device_desc(NULL, 0, &d) < 0);
	KT_EQ(rs_usb_max_packet0(devd, 8), 8);
	static const uint8_t bad_cfg[] = { 9, 2, 200, 0, 1, 1, 0, 0xa0, 50, 0, 0, 9, 4 };
	struct rs_usb_iface k;
	KT_ASSERT(rs_usb_find_boot_kbd(bad_cfg, sizeof(bad_cfg), &k) < 0);
	KT_ASSERT(rs_usb_find_boot_mouse(bad_cfg, sizeof(bad_cfg), &k) < 0);
	KT_ASSERT(rs_usb_find_hub(bad_cfg, sizeof(bad_cfg), &k) < 0);
	KT_EQ(rs_usb_config_total(bad_cfg, sizeof(bad_cfg)), 200);
	uint8_t out[4];
	KT_EQ(rs_usb_kbd_report(USB_MAX_KBDS - 1, bad_cfg, 2, out, sizeof(out)), 0); /* too short */
	static const uint8_t a_key[8] = { 0, 0, 4, 0, 0, 0, 0, 0 };
	KT_EQ(rs_usb_kbd_report(USB_MAX_KBDS, a_key, 8, out, sizeof(out)), 0); /* no such keyboard */
	static const uint8_t hubd[9] = { 9, 0x29, 4, 0x69, 0, 50, 0, 0, 0xff };
	KT_EQ(rs_usb_hub_desc(hubd, 9, 0), 4 | 3 << 8 | 100 << 16);
	KT_EQ(rs_usb_hub_desc(hubd, 6, 0), -1);
	KT_EQ(rs_usb_hub_desc(hubd, 9, 1), -1); /* not a SuperSpeed hub descriptor */
	static const uint8_t ps[4] = { 0x03, 0x05, 0x11, 0x00 };
	KT_EQ(rs_usb_port_status(ps, 4), 0x0503 | 0x11 << 16);
	KT_EQ(rs_usb_port_status(ps, 3), -1);
	static const uint8_t mr[4] = { 1, 0xfe, 5, 0 };
	KT_EQ(rs_usb_mouse_report(mr, 4), 1 | 0xfe << 8 | 5 << 16);
	KT_EQ(rs_usb_mouse_report(mr, 2), -1);
}

/* Review F1: a ring page holds 255 TRBs and a link TRB; 300 No Op
 * commands, one at a time, must all complete, so the command ring wraps
 * at least once (QEMU's xHCI checks the link TRB's cycle and TC bits). */
KTEST(073, xhci_command_ring_wraps)
{
	const struct xhci_stats *s = &xhci_hc[0];
	KT_ASSERT(s->running && s->vector);
	int done = 0;
	for (int i = 0; i < 300; i++) {
		uint64_t cmds = s->cmd_events;
		if (xhci_noop(1) != 0)
			break;
		for (int t = 0; t < 200 && s->cmd_events == cmds; t++)
			thread_sleep_ms(1);
		if (s->cmd_events != cmds + 1)
			break;
		done++;
	}
	KT_EQ(done, 300);
	KT_EQ(s->bad_events, 0);
}

/* Review F4: one interrupt handles at most 32 events. 40 No Op commands
 * behind one doorbell complete at once; the handler leaves 8, and the
 * controller interrupts again because the ring is not empty when the
 * handler clears EHB. All 40 arrive, over at least two interrupts. */
KTEST(074, xhci_irq_handles_a_bounded_batch)
{
	const struct xhci_stats *s = &xhci_hc[0];
	KT_ASSERT(s->running && s->vector);
	uint64_t irqs = s->irqs, cmds = s->cmd_events;
	KT_EQ(xhci_noop(40), 0);
	for (int t = 0; t < 500 && s->cmd_events < cmds + 40; t++)
		thread_sleep_ms(1);
	KT_EQ(s->cmd_events, cmds + 40);
	KT_ASSERT(s->irqs >= irqs + 2);
	KT_ASSERT(s->irq_max_events >= 1 && s->irq_max_events <= 32);
	KT_EQ(s->bad_events, 0);
	KT_EQ(xhci_noop(0), -EINVAL);
	KT_EQ(xhci_noop(65), -EINVAL);
}

/* Review F6 (control endpoint): a request QEMU's keyboard does not know
 * is answered with STALL, which halts EP0 in the controller. The driver
 * must Reset Endpoint (QEMU refuses that unless the endpoint is halted)
 * and Set TR Dequeue Pointer, after which a normal request works. */
KTEST(075, usb_ep0_stall_recovers)
{
	const struct usb_info *k = first_kbd();
	KT_ASSERT(k != NULL);
	if (!k)
		return;
	struct usb_test_req r = { .hc = k->hc, .slot = k->slot };
	uint64_t resets = usb.ep0_resets;
	KT_EQ(usb_call(usb_test_stall_ep0, &r), 0);
	KT_EQ(r.result, 0); /* the stalled request failed and GET_DESCRIPTOR after it worked */
	KT_EQ(usb.ep0_resets, resets + 1);
	KT_EQ(r.desc.vendor, 0x0627);
	KT_EQ(usb_ep_state(k->hc, k->slot, 1), XHCI_EP_RUNNING);
}

/* Review F6 (interrupt endpoint): a second interrupt-IN endpoint (EP 2 IN,
 * which QEMU's keyboard answers with STALL) is added to the keyboard. Each
 * stall halts it; the driver resets it, clears the device's halt, sets the
 * dequeue pointer and queues again (QEMU checks every step's endpoint
 * state), and gives up after 3 errors in a row. The keyboard's own
 * endpoint is not affected. */
KTEST(076, usb_interrupt_ep_stall_recovers_then_gives_up)
{
	const struct usb_info *k = first_kbd();
	KT_ASSERT(k != NULL);
	if (!k)
		return;
	int hc = k->hc, slot = k->slot;
	uint64_t errs = usb.ep_errors, resets = usb.ep_resets;
	unsigned dead = usb.ep_dead;
	struct usb_test_req r = { .hc = hc, .slot = slot };
	KT_EQ(usb_call(usb_test_stalling_ep, &r), 0);
	KT_EQ(r.result, 0);
	for (int t = 0; t < 2000 && usb.ep_dead == dead; t++)
		thread_sleep_ms(1);
	KT_EQ(usb.ep_dead, dead + 1);
	KT_EQ(usb.ep_errors, errs + 3);
	KT_EQ(usb.ep_resets, resets + 3); /* each Reset Endpoint + Set TR Dequeue succeeded */
	KT_EQ(usb_ep_state(hc, slot, 5), XHCI_EP_STOPPED); /* reset, not queued again */
	KT_EQ(usb_ep_state(hc, slot, 3), XHCI_EP_RUNNING); /* the keyboard endpoint */
	k = first_kbd();
	KT_ASSERT(k && k->ep_errors == 0 && !k->ep_dead);
}

/* Route strings and the Transaction Translator fields on made-up trees,
 * including the desktop's (a full-speed keyboard behind a high-speed
 * Genesys hub) which QEMU cannot build: its usb-hub is full speed. */
KTEST(077, usb_route_and_tt_rules)
{
	uint8_t s, p;
	bool m;
	/* root port device: route 0, no TT even at full speed (the root hub has its own) */
	usb_tt(NULL, 6, XHCI_SPEED_FULL, &s, &p, &m);
	KT_ASSERT(s == 0 && p == 0 && !m);
	KT_EQ(usb_route(NULL, 6), 0);
	/* high-speed single-TT hub in slot 4 on root port 6; keyboard on its port 2 */
	struct usb_topo hs = { NULL, 4, XHCI_SPEED_HIGH, 6, 0, false, 0 };
	KT_EQ(usb_route(&hs, 2), 0x2);
	usb_tt(&hs, 2, XHCI_SPEED_FULL, &s, &p, &m);
	KT_ASSERT(s == 4 && p == 2 && !m);
	usb_tt(&hs, 3, XHCI_SPEED_LOW, &s, &p, &m);
	KT_ASSERT(s == 4 && p == 3 && !m);
	usb_tt(&hs, 2, XHCI_SPEED_HIGH, &s, &p, &m); /* high speed needs no TT */
	KT_ASSERT(s == 0 && p == 0 && !m);
	/* a multi-TT hub passes MTT on */
	struct usb_topo mtt = { NULL, 7, XHCI_SPEED_HIGH, 1, 0, true, 0 };
	usb_tt(&mtt, 5, XHCI_SPEED_FULL, &s, &p, &m);
	KT_ASSERT(s == 7 && p == 5 && m);
	/* full-speed hub (slot 9) on port 4 of the high-speed hub: the TT is
	 * the high-speed hub's port 4, wherever the keyboard sits below */
	struct usb_topo fs = { &hs, 9, XHCI_SPEED_FULL, 4, 1, false, usb_route(&hs, 4) };
	KT_EQ(fs.route, 0x4);
	KT_EQ(usb_route(&fs, 3), 0x34);
	usb_tt(&fs, 3, XHCI_SPEED_FULL, &s, &p, &m);
	KT_ASSERT(s == 4 && p == 4 && !m);
	/* a full-speed hub on a root port (QEMU's case): no TT anywhere */
	struct usb_topo qfs = { NULL, 2, XHCI_SPEED_FULL, 3, 0, false, 0 };
	usb_tt(&qfs, 1, XHCI_SPEED_FULL, &s, &p, &m);
	KT_ASSERT(s == 0 && p == 0);
	KT_EQ(usb_route(&qfs, 1), 0x1);
	/* ports above 15 do not fit a route nibble; depth 5 is the limit */
	KT_EQ(usb_route(&hs, 16), 0xffffffffu);
	struct usb_topo deep = { NULL, 1, XHCI_SPEED_HIGH, 1, 5, false, 0x11111 };
	KT_EQ(usb_route(&deep, 1), 0xffffffffu);
}

/* The desktop-like topology: devices on both controllers, a keyboard
 * behind a hub, and the slot contexts the controller keeps. */
KTEST(078, usb_desktop_topology)
{
	if (!cmdline_has("kt-desktop")) {
		KT_ASSERT(true);
		return;
	}
	KT_EQ(usb.hubs, 1);
	KT_EQ(usb.keyboards, 2);
	KT_EQ(usb.mice, 1);
	const struct usb_info *hub = find("7", 1);
	KT_ASSERT(hub && hub->hub && hub->hub_ports == 8 && hub->speed == XHCI_SPEED_FULL);
	if (!hub)
		return;
	/* the hub fields go out with Configure Endpoint; QEMU keeps only
	 * Context Entries of that slot context, so check what Magia sent */
	KT_ASSERT(hub->sctx_sent[0] & XHCI_SCTX_HUB(1));
	KT_EQ((hub->sctx_sent[1] >> 24) & 0xff, 8); /* number of ports */
	KT_EQ((hub->sctx_sent[1] >> 16) & 0xff, 7); /* root port */
	KT_EQ(hub->sctx_sent[2], 0);                /* no TT fields: a full-speed hub */
	KT_EQ((hub->sctx_sent[0] >> 27) & 0x1f, 3); /* context entries: EP 1 IN */
	uint32_t sc[4];
	KT_EQ(usb_slot_ctx(1, hub->slot, sc), 0);
	KT_EQ((sc[0] >> 27) & 0x1f, 3);
	KT_EQ(XHCI_SCTX_SLOT_STATE(sc[3]), 3); /* configured */
	const struct usb_info *kh = find("7.1", 1);
	KT_ASSERT(kh && kh->kbd && kh->vendor == 0x0627);
	if (kh) {
		KT_EQ(kh->route, 1);
		KT_EQ(kh->rootport, 7);
		KT_EQ(kh->depth, 1);
		KT_EQ(kh->tt_slot, 0); /* a full-speed hub has no TT */
		KT_EQ(usb_slot_ctx(1, kh->slot, sc), 0);
		KT_EQ(sc[0] & 0xfffff, 1);
		KT_EQ((sc[1] >> 16) & 0xff, 7);
		KT_EQ(usb_ep_state(1, kh->slot, kh->kbd_dci), XHCI_EP_RUNNING);
	}
	const struct usb_info *kr = find("8", 1);
	KT_ASSERT(kr && kr->kbd && kr->route == 0 && kr->depth == 0);
	const struct usb_info *ms = find("5", 0);
	KT_ASSERT(ms && ms->mouse && !ms->kbd);
	KT_EQ(xhci_hc[0].bad_events + xhci_hc[1].bad_events, 0);
}

/* Speed names for every Protocol Speed ID a root port reports, including
 * SuperSpeedPlus (5 on Intel 800-series ports), which must not fall back
 * to full-speed rules. */
KTEST(079, usb_speed_ids)
{
	KT_EQ(strcmp(usb_speed_name(XHCI_SPEED_FULL), "FS"), 0);
	KT_EQ(strcmp(usb_speed_name(XHCI_SPEED_LOW), "LS"), 0);
	KT_EQ(strcmp(usb_speed_name(XHCI_SPEED_HIGH), "HS"), 0);
	KT_EQ(strcmp(usb_speed_name(XHCI_SPEED_SUPER), "SS"), 0);
	KT_EQ(strcmp(usb_speed_name(5), "SS+"), 0);
	KT_EQ(strcmp(usb_speed_name(0), "?"), 0);
	KT_EQ(strcmp(usb_speed_name(200), "?"), 0);
}

/* Review 2b L5: connect attempts per port are rate-limited (5 per 10 s;
 * a failed attempt costs two), so a flapping port or hub cannot keep the
 * usb thread busy; the window then opens again. */
KTEST(081, usb_connect_rate_limit)
{
	struct usb_rate r = { 0 };
	uint64_t t = 123456;
	for (int i = 0; i < USB_CONNECT_MAX; i++)
		KT_ASSERT(usb_connect_allowed(&r, t + (uint64_t)i));
	KT_ASSERT(!usb_connect_allowed(&r, t + 10));
	KT_ASSERT(!usb_connect_allowed(&r, t + USB_CONNECT_WINDOW_MS - 1));
	KT_ASSERT(usb_connect_allowed(&r, t + USB_CONNECT_WINDOW_MS)); /* a new window */
	/* failures back off faster: each failed attempt counts twice */
	struct usb_rate f = { 0 };
	int ok = 0;
	for (int i = 0; i < 10; i++)
		if (usb_connect_allowed(&f, 5000)) {
			ok++;
			usb_connect_failed(&f);
		}
	KT_EQ(ok, (USB_CONNECT_MAX + 1) / 2);
	/* time going backwards (TSC reset) does not lock a port out */
	struct usb_rate b = { 0 };
	for (int i = 0; i < USB_CONNECT_MAX; i++)
		usb_connect_allowed(&b, 50000);
	KT_ASSERT(usb_connect_allowed(&b, 100));
}

/* Review 2b L4: when Disable Slot fails the device's pages cannot be
 * freed (the controller may still use them). They are counted, and after
 * 3 failures the controller attaches nothing more (logged) instead of
 * leaking memory under a flapping device. The hook really disables the
 * slot but treats it as failed, then re-attaches the keyboard. Runs last
 * among the USB tests: it leaves that controller without a keyboard. */
KTEST(089, usb_disable_slot_failures_stop_attaching)
{
	int n = usb_list(devs, MAXDEV);
	const struct usb_info *k = NULL;
	for (int i = 0; i < n && !k; i++)
		if (devs[i].kbd && devs[i].depth == 0)
			k = &devs[i];
	KT_ASSERT(k != NULL);
	if (!k)
		return;
	uint64_t leaked = usb.leaked_pages;
	struct usb_test_req r = { .hc = k->hc, .slot = k->slot };
	KT_EQ(usb_call(usb_test_disable_failures, &r), 0);
	KT_EQ(r.result, 2);                      /* re-attached after the 1st and 2nd failure, not the 3rd */
	KT_EQ(usb.disable_failures, 3);
	KT_ASSERT(r.pages >= 3 * 4);            /* context, EP0 ring, keyboard ring and buffer, three times */
	KT_EQ(usb.leaked_pages - leaked, r.pages);
	KT_EQ(usb.attach_refused, 1);
	KT_ASSERT(kt_log_has(0, "Disable Slot failed 3 times"));
}
