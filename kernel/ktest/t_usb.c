/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* t_usb.c - ktests for the xHCI driver and the USB boot keyboard (07x). */
#include <magia/apic.h>
#include <magia/ktest.h>
#include <magia/pci.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/usb.h>

/* QEMU qemu-xhci + usb-kbd: the controller runs, interrupts come by
 * MSI-X, and a command completes through the interrupt path. */
KTEST(070, xhci_controller_runs_with_msix)
{
	KT_ASSERT(xhci.running);
	KT_EQ(xhci.nports, 8);
	KT_ASSERT(xhci.nslots >= 1);
	KT_ASSERT(xhci.csz == 32 || xhci.csz == 64);
	KT_ASSERT(xhci.msix);
	KT_ASSERT(xhci.vector >= VEC_MSI_FIRST && xhci.vector <= VEC_MSI_LAST);
	KT_ASSERT(xhci.cmd_events >= 3); /* enable slot, address device, configure endpoint */
	KT_EQ(xhci.bad_events, 0);
	uint64_t irqs = xhci.irqs, cmds = xhci.cmd_events, vec = vec_counts[xhci.vector];
	KT_EQ(xhci_noop(), 0);
	for (int i = 0; i < 200 && xhci.cmd_events == cmds; i++)
		thread_sleep_ms(1);
	KT_EQ(xhci.cmd_events, cmds + 1);
	KT_ASSERT(xhci.irqs > irqs);
	KT_ASSERT(vec_counts[xhci.vector] > vec);
}

/* Review F1: a ring page holds 255 TRBs and a link TRB; 300 No Op
 * commands, one at a time, must all complete, so the command ring wraps
 * at least once (QEMU's xHCI checks the link TRB's cycle and TC bits). */
KTEST(073, xhci_command_ring_wraps)
{
	KT_ASSERT(xhci.running && xhci.vector);
	int done = 0;
	for (int i = 0; i < 300; i++) {
		uint64_t cmds = xhci.cmd_events;
		if (xhci_noop() != 0)
			break;
		for (int t = 0; t < 200 && xhci.cmd_events == cmds; t++)
			thread_sleep_ms(1);
		if (xhci.cmd_events != cmds + 1)
			break;
		done++;
	}
	KT_EQ(done, 300);
	KT_EQ(xhci.bad_events, 0);
}

KTEST(071, usb_boot_keyboard_attached)
{
	KT_ASSERT(xhci.kbd);
	KT_EQ(xhci.kbd_dev.vendor, 0x0627); /* QEMU's usb-kbd */
	KT_EQ(xhci.kbd_dev.product, 0x0001);
	KT_EQ(xhci.kbd_if.ep_addr, 0x81);
	KT_EQ(xhci.kbd_if.ep_max_packet, 8);
	KT_EQ(xhci.kbd_dci, 3); /* endpoint 1 IN */
	KT_ASSERT(xhci.kbd_port >= 1 && xhci.kbd_port <= xhci.nports);
	KT_EQ(xhci.kbd_errors, 0);
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
	struct rs_usb_bootkbd k;
	KT_ASSERT(rs_usb_find_boot_kbd(bad_cfg, sizeof(bad_cfg), &k) < 0);
	KT_EQ(rs_usb_config_total(bad_cfg, sizeof(bad_cfg)), 200);
	uint8_t out[4];
	KT_EQ(rs_usb_kbd_report(bad_cfg, 2, out, sizeof(out)), 0); /* too short */
}
