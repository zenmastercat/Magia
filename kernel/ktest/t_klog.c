/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* t_klog.c - ktests for the kernel log, the framebuffer scroll-back, the
 * panic screen and hwinfo (034-039). */
#include <magia/fbcon.h>
#include <magia/kbd.h>
#include <magia/klog.h>
#include <magia/ktest.h>
#include <magia/x86.h>

int monitor_exec(char *line);

/* Does the kernel log contain `s` at or after position `from`? */
bool kt_log_has(uint64_t from, const char *s)
{
	size_t n = strlen(s);
	char buf[128];
	if (n == 0 || n > sizeof(buf))
		return false;
	for (uint64_t p = from; p + n <= kernel_log.head; p++)
		if (klog_read(&kernel_log, p, buf, n) == n && memcmp(buf, s, n) == 0)
			return true;
	return false;
}

/* hwinfo lists the PCI functions and the USB devices: QEMU's xHCI and
 * usb-kbd are on it, and so is the machine line. */
KTEST(034, hwinfo_lists_pci_and_usb)
{
	uint64_t h = kernel_log.head;
	char cmd[] = "hwinfo";
	KT_EQ(monitor_exec(cmd), 0);
	KT_ASSERT(kt_log_has(h, "hwinfo | "));
	KT_ASSERT(kt_log_has(h, "1b36:000d 0c0330")); /* the xHCI, class 0c/03/30 */
	KT_ASSERT(kt_log_has(h, "8086:29c0 060000")); /* q35 host bridge */
	KT_ASSERT(kt_log_has(h, "0627:0001 FS kbd") || kt_log_has(h, "0627:0001 HS kbd"));
	h = kernel_log.head;
	char cmd2[] = "dmesg 2";
	KT_EQ(monitor_exec(cmd2), 0);
	KT_EQ(kernel_log.head, h); /* dmesg output itself is not logged */
	char bad[] = "dmesg x";
	KT_EQ(monitor_exec(bad), 0); /* prints usage */
}

/* A small ring: wrapping, the partial first line dropped, tails. */
KTEST(035, klog_ring_wraps_and_tails)
{
	static char buf[64];
	struct klog l = { buf, sizeof(buf), 0 };
	char out[128];
	KT_EQ(klog_first(&l), 0);
	KT_EQ(klog_read(&l, 0, out, sizeof(out)), 0);
	klog_put(&l, "one\ntwo\n", 8);
	KT_EQ(l.head, 8);
	KT_EQ(klog_read(&l, 0, out, sizeof(out)), 8);
	KT_EQ(memcmp(out, "one\ntwo\n", 8), 0);
	KT_EQ(klog_tail(&l, 1), 4); /* "two\n" */
	KT_EQ(klog_tail(&l, 2), 0);
	KT_EQ(klog_tail(&l, 9), 0);
	klog_put(&l, "par", 3); /* an unfinished line counts as a line */
	KT_EQ(klog_tail(&l, 1), 8);
	KT_EQ(klog_tail(&l, 2), 4);
	/* 100 more bytes: the ring keeps the last 64, starting after a newline */
	for (int i = 0; i < 10; i++)
		klog_put(&l, "line-xxxx\n", 10);
	KT_EQ(l.head, 111);
	uint64_t first = klog_first(&l);
	KT_ASSERT(first >= l.head - sizeof(buf));
	KT_EQ(klog_read(&l, first - 1, out, 1), 1); /* the byte before the first line ... */
	KT_EQ(out[0], '\n');                          /* ... is a newline, if still in the ring */
	size_t n = klog_read(&l, 0, out, sizeof(out)); /* reading from 0 clamps to the ring */
	KT_EQ(n, sizeof(buf));
	/* one write longer than the ring keeps only its end */
	char big[200];
	for (int i = 0; i < 200; i++)
		big[i] = (char)('a' + i % 26);
	big[199] = '\n';
	klog_put(&l, big, sizeof(big));
	KT_EQ(l.head, 311);
	KT_EQ(klog_read(&l, l.head - 64, out, 64), 64);
	KT_EQ(memcmp(out, big + 136, 64), 0);
	KT_EQ(klog_first(&l), l.head); /* no newline inside the ring before the last byte */
	/* the kernel's own log holds this boot's banner */
	KT_ASSERT(kernel_log.head > 100);
	char head[64];
	n = klog_read(&kernel_log, klog_first(&kernel_log), head, sizeof(head));
	KT_ASSERT(n > 0);
}

/* The kernel log sees console output; dmesg's own output is not logged. */
KTEST(036, klog_records_console_output)
{
	uint64_t h = kernel_log.head;
	kprintf("klog-probe-%d\n", 4711);
	KT_EQ(kernel_log.head, h + 16);
	char out[16];
	KT_EQ(klog_read(&kernel_log, h, out, 16), 16);
	KT_EQ(memcmp(out, "klog-probe-4711\n", 16), 0);
	h = kernel_log.head;
	klog_dump_tail(3); /* prints the last 3 lines without logging them */
	KT_EQ(kernel_log.head, h);
}

/* Page Up / Page Down: lines that scrolled off come back, output snaps to
 * the bottom. */
KTEST(037, fbcon_scrollback)
{
	KT_ASSERT(fbcon_active());
	if (!fbcon_active())
		return;
	int rows = fbcon_rows;
	uint64_t f = irq_save();
	for (int i = 0; i < rows; i++) /* enough history above the mark */
		kprintf("filler %d\n", i);
	kprintf("scroll-mark-A\n");
	for (int i = 0; i < 2 * rows; i++)
		kprintf("filler %d\n", i);
	KT_EQ(fbcon_scrolled(), 0);
	bool on_screen = false;
	for (int r = 0; r < rows; r++)
		on_screen |= fbcon_char_at(0, r) == 's' && fbcon_char_at(7, r) == 'm';
	KT_ASSERT(!on_screen); /* scroll-mark-A has left the screen */
	fbcon_scroll(rows - 1);
	KT_EQ(fbcon_scrolled(), rows - 1);
	fbcon_scroll(rows - 1);
	KT_EQ(fbcon_scrolled(), 2 * (rows - 1));
	on_screen = false;
	for (int r = 0; r < rows; r++)
		on_screen |= fbcon_char_at(0, r) == 's' && fbcon_char_at(12, r) == 'A';
	KT_ASSERT(on_screen); /* back in view two pages up */
	fbcon_scroll(-(rows - 1));
	KT_EQ(fbcon_scrolled(), rows - 1);
	fbcon_scroll(1 << 20); /* clamped to the history */
	KT_ASSERT(fbcon_scrolled() <= fbcon_history());
	KT_ASSERT(fbcon_history() >= rows + 5);
	fbcon_scroll(-(1 << 20));
	KT_EQ(fbcon_scrolled(), 0);
	fbcon_scroll(5);
	kprintf("snap\n"); /* new output returns to the live view */
	KT_EQ(fbcon_scrolled(), 0);
	irq_restore(f);
}

/* Page Up/Down from the PS/2 keyboard, the USB keyboard (Rust, host
 * tests) and serial terminals all become ESC [5~ / ESC [6~, which the
 * monitor decodes. */
KTEST(038, page_keys_decode)
{
	struct kbd_state st = { 0 };
	kbd_translate(&st, 0xe0);
	KT_EQ(kbd_translate(&st, 0x49), KBD_PGUP);
	kbd_translate(&st, 0xe0);
	KT_EQ(kbd_translate(&st, 0x51), KBD_PGDN);
	kbd_translate(&st, 0xe0);
	KT_EQ(kbd_translate(&st, 0xc9), -1); /* release */
	struct vt_dec d = { 0 };
	const char *seq = "a\x1b[5~b\x1b[6~\x1b[A\x1b" "c";
	int got[16], n = 0;
	for (const char *p = seq; *p && n < 16; p++) {
		int c = vt_decode(&d, (uint8_t)*p);
		if (c >= 0)
			got[n++] = c;
	}
	KT_EQ(n, 4);
	KT_EQ(got[0], 'a');
	KT_EQ(got[1], VT_PGUP);
	KT_EQ(got[2], 'b');
	KT_EQ(got[3], VT_PGDN); /* ESC [A (arrow) and ESC c are swallowed */
	/* a long run of digits cannot overflow the decoder */
	d = (struct vt_dec){ 0 };
	vt_decode(&d, 0x1b);
	vt_decode(&d, '[');
	for (int i = 0; i < 100; i++)
		KT_EQ(vt_decode(&d, '9'), -1);
	KT_EQ(vt_decode(&d, '~'), -1);
	KT_EQ(vt_decode(&d, 'x'), 'x');
}

/* The panic report: header on row 0 in white on red, the last log lines
 * below it, ending with the newest one. */
KTEST(039, fbcon_panic_screen)
{
	KT_ASSERT(fbcon_active());
	if (!fbcon_active())
		return;
	uint64_t f = irq_save();
	kprintf("panic-screen-probe-last\n");
	fbcon_panic_screen("ktest probe");
	bool hdr = true;
	static const char want[] = "MAGIA PANIC";
	for (int i = 0; want[i]; i++)
		hdr &= fbcon_char_at(1 + i, 0) == (uint8_t)want[i];
	KT_ASSERT(hdr);
	KT_EQ(fbcon_pixel(0, 0), fbcon_rgb(0xb0, 0, 0)); /* red background */
	int last = -1;
	for (int r = 0; r < fbcon_rows; r++)
		if (fbcon_char_at(0, r) == 'p' && fbcon_char_at(19, r) == 'l')
			last = r;
	KT_ASSERT(last > 1);
	KT_EQ(fbcon_scrolled(), 0);
	kprintf("after the panic screen the console goes on\n");
	irq_restore(f);
}
