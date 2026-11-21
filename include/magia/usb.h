/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * usb.h - the xHCI driver (kernel/xhci.c) and the Rust parsers for data
 * USB devices send (rust/src/usb.rs). Layouts must match the #[repr(C)]
 * structs in rust/src/usb.rs.
 */
#ifndef MAGIA_USB_H
#define MAGIA_USB_H

#include <magia/kernel.h>

struct rs_usb_dev {
	uint16_t usb_version, vendor, product;
	uint8_t class, subclass, protocol, max_packet0, num_configs;
	uint8_t _pad[3];
};

struct rs_usb_bootkbd {
	uint8_t config_value, interface, alt_setting, ep_addr;
	uint16_t ep_max_packet;
	uint8_t ep_interval, _pad;
};

/* Rust (rust/src/usb.rs): each returns >= 0 on success, < 0 on bad input. */
int32_t rs_usb_max_packet0(const uint8_t *b, size_t n);
int32_t rs_usb_device_desc(const uint8_t *b, size_t n, struct rs_usb_dev *out);
int32_t rs_usb_config_total(const uint8_t *b, size_t n);
int32_t rs_usb_find_boot_kbd(const uint8_t *b, size_t n, struct rs_usb_bootkbd *out);
/* One keyboard's report -> ASCII for newly pressed keys; returns bytes written. */
size_t rs_usb_kbd_report(const uint8_t *r, size_t n, uint8_t *out, size_t outlen);

/* xhci.c */
struct xhci_stats {
	bool running;            /* controller up */
	uint8_t nports, nslots, csz;
	bool msix;               /* interrupts by MSI-X (else MSI, else polling) */
	int vector;
	uint64_t events, cmd_events, xfer_events, port_events, irqs, bad_events;
	unsigned irq_max_events; /* most events one interrupt handled */
	/* the keyboard, if one was found */
	bool kbd;
	uint8_t kbd_port, kbd_slot, kbd_speed, kbd_dci;
	struct rs_usb_dev kbd_dev;
	struct rs_usb_bootkbd kbd_if;
	uint64_t kbd_reports, kbd_chars, kbd_errors;
	uint16_t devices;        /* devices seen on root ports */
};
extern struct xhci_stats xhci;
void xhci_dump(void); /* monitor "usb" */
int xhci_noop(unsigned n); /* ktest: n No Op commands (1..64) completed by interrupt */

#endif
