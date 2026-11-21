/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * usb.h - the xHCI driver with its hub, keyboard and mouse class code
 * (kernel/xhci.c) and the Rust parsers for data USB devices send
 * (rust/src/usb.rs, rust/src/usbhub.rs). Layouts must match the
 * #[repr(C)] structs in rust/src/ffi.rs.
 */
#ifndef MAGIA_USB_H
#define MAGIA_USB_H

#include <magia/kernel.h>

struct rs_usb_dev {
	uint16_t usb_version, vendor, product;
	uint8_t class, subclass, protocol, max_packet0, num_configs;
	uint8_t _pad[3];
};

/* One interface with an interrupt-IN endpoint: boot keyboard, boot mouse
 * or hub (`extra` = the hub's multi-TT alternate setting, 0xff: none). */
struct rs_usb_iface {
	uint8_t config_value, interface, alt_setting, ep_addr;
	uint16_t ep_max_packet;
	uint8_t ep_interval, extra;
};

/* Rust: each returns >= 0 on success, < 0 on bad input. */
int32_t rs_usb_max_packet0(const uint8_t *b, size_t n);
int32_t rs_usb_device_desc(const uint8_t *b, size_t n, struct rs_usb_dev *out);
int32_t rs_usb_config_total(const uint8_t *b, size_t n);
int32_t rs_usb_find_boot_kbd(const uint8_t *b, size_t n, struct rs_usb_iface *out);
int32_t rs_usb_find_boot_mouse(const uint8_t *b, size_t n, struct rs_usb_iface *out);
int32_t rs_usb_find_hub(const uint8_t *b, size_t n, struct rs_usb_iface *out);
/* nports | tt_think << 8 | ss << 10 | power_on_ms << 16 */
int64_t rs_usb_hub_desc(const uint8_t *b, size_t n, int32_t ss);
/* wPortStatus | wPortChange << 16 */
int64_t rs_usb_port_status(const uint8_t *b, size_t n);
/* buttons | dx << 8 | dy << 16 | wheel << 24 (dx, dy, wheel: int8) */
int64_t rs_usb_mouse_report(const uint8_t *r, size_t n);
/* Keyboard `id` (0..USB_MAX_KBDS-1): report -> ASCII for newly pressed
 * keys and VT220 sequences for Page Up/Down; returns bytes written. */
size_t rs_usb_kbd_report(uint32_t id, const uint8_t *r, size_t n, uint8_t *out, size_t outlen);
void rs_usb_kbd_reset(uint32_t id);
#define USB_MAX_KBDS 8

/* xhci.c: one per controller */
#define USB_MAX_HC 4
struct xhci_stats {
	bool running;            /* controller up */
	bool msix;               /* interrupts by MSI-X (else MSI, else polling) */
	int vector;
	uint8_t nports, nslots, csz;
	uint8_t bus, dev, fn;
	uint16_t vendor, device;
	uint64_t events, cmd_events, xfer_events, port_events, irqs;
	uint64_t bad_events;     /* for an endpoint the device does not have */
	uint64_t stale_events;   /* for a slot or endpoint already gone or halted */
	unsigned irq_max_events; /* most events one interrupt handled */
};
extern struct xhci_stats xhci_hc[USB_MAX_HC];
extern int xhci_count;

struct usb_stats {
	bool boot_done;          /* the usb thread finished the first scan */
	unsigned attached, detached, devices, keyboards, mice, hubs;
	uint64_t kbd_reports, kbd_chars, mouse_reports;
	int64_t mouse_x, mouse_y;
	uint32_t mouse_buttons;  /* buttons held in the newest report */
	uint64_t ep_errors;      /* transfer errors on interrupt endpoints */
	uint64_t ep_resets;      /* endpoints recovered (Reset Endpoint / Set TR Dequeue) */
	uint64_t ep0_resets;     /* the same for control endpoints */
	unsigned ep_dead;        /* endpoints given up after repeated errors */
	unsigned disable_failures; /* Disable Slot commands that failed */
	uint64_t leaked_pages;   /* pages kept because the controller may still use them */
	unsigned attach_refused; /* attaches refused on a controller with too many failures */
	unsigned connects_limited; /* connect attempts deferred by the rate limit */
};

/* Connect attempts per port (review 2b L5): at most USB_CONNECT_MAX per
 * USB_CONNECT_WINDOW_MS, a failed attempt counting twice; then the port
 * waits for the next window. */
#define USB_CONNECT_MAX 5
#define USB_CONNECT_WINDOW_MS 10000
struct usb_rate {
	uint64_t start; /* ms: window start */
	uint8_t count, logged;
};
bool usb_connect_allowed(struct usb_rate *r, uint64_t now_ms); /* takes one attempt if allowed */
void usb_connect_failed(struct usb_rate *r);                    /* the attempt failed: one more */
bool usb_connect_open(const struct usb_rate *r, uint64_t now_ms); /* would an attempt be allowed? */
extern struct usb_stats usb;

/* One attached device, for `usb`, hwinfo and ktest. */
struct usb_info {
	uint8_t hc, slot, speed, rootport, port, depth;
	char path[24];           /* "3.1": root port, then hub ports */
	uint32_t route;
	uint8_t tt_slot, tt_port;
	bool mtt, hub, kbd, mouse;
	uint16_t vendor, product;
	uint8_t class, hub_ports, hub_ttt;
	uint8_t kbd_dci, kbd_ep;
	uint16_t kbd_mps;
	uint64_t kbd_reports, mouse_reports, ep_errors, ep_resets;
	uint32_t sctx_sent[4];   /* the slot context Magia sent last (Address Device, Configure Endpoint) */
	bool ep_dead;
};
int usb_list(struct usb_info *out, int max);       /* number of devices */
int usb_slot_ctx(int hc, int slot, uint32_t out[4]); /* output slot context, or -errno */
int usb_ep_state(int hc, int slot, int dci);       /* XHCI_EP_* from the output context */
const char *usb_speed_name(uint8_t speed);         /* "FS", "LS", "HS", "SS" */

void xhci_dump(void);      /* monitor "usb" */
int xhci_noop(unsigned n); /* ktest: n No Op commands (1..64) on controller 0, completed by interrupt */
int usb_call(void (*fn)(void *), void *arg); /* run fn in the usb thread and wait (ktest) */

/* The topology a slot context needs, kept apart so ktest can check the
 * route string and Transaction Translator rules on made-up trees. */
struct usb_topo {
	const struct usb_topo *parent; /* hub above, NULL on a root port */
	uint8_t slot, speed, port;     /* port: on the parent hub, or the root port */
	uint8_t depth;                 /* external hubs above (0 on a root port) */
	bool hub_mtt;                  /* a high-speed hub running its multi-TT interface */
	uint32_t route;
};
/* Route string of a device on `port` of hub `parent` (USB 3.2 8.9). */
uint32_t usb_route(const struct usb_topo *parent, uint8_t port);
/* TT hub slot, TT port and MTT for a device of `speed` on `port` of
 * `parent`: the nearest high-speed hub upstream, if the device is full or
 * low speed (xHCI 6.2.2); zeros otherwise. */
void usb_tt(const struct usb_topo *parent, uint8_t port, uint8_t speed, uint8_t *tt_slot, uint8_t *tt_port,
	    bool *mtt);

/* ktest hooks, run through usb_call() */
struct usb_test_req {
	int hc, slot, result;
	struct rs_usb_dev desc; /* read back after the stall */
	uint64_t pages;         /* pages the detached devices held */
};
void usb_test_disable_failures(void *req); /* detach a root keyboard 3 times with "failing" Disable Slot */
void usb_test_stall_ep0(void *req);   /* a request QEMU stalls, then GET_DESCRIPTOR */
void usb_test_stalling_ep(void *req); /* add an interrupt-IN endpoint QEMU stalls */

#endif
