/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* hwinfo.h - the `hwinfo` screen (kernel/hwinfo.c): machine, PCI and USB
 * IDs packed into columns, so one photograph of the screen holds them. */
#ifndef MAGIA_HWINFO_H
#define MAGIA_HWINFO_H

#include <magia/kernel.h>

void hwinfo(void);
/* One entry of a packed list (at most HWINFO_W - 2 characters). */
#define HWINFO_W 28
void hwinfo_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void usb_hwinfo(void); /* xhci.c: one hwinfo_add() per controller and device */

#endif
