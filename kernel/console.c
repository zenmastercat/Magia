/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * console.c - COM1 (16550 UART) console and console input.
 *
 * Serial is the primary console (QEMU -nographic, the test harness and the
 * magia> monitor all use it); output is copied to the framebuffer console
 * (fbcon.c) and kept in the kernel log ring (klog.c). Input comes from
 * COM1, the PS/2 keyboard (kbd.c) and USB keyboards (xhci.c).
 * Input arrives on IRQ4 into a small ring buffer; console_getc_wait()
 * sleeps until a byte is available.
 */
#include <magia/fbcon.h>
#include <magia/kbd.h>
#include <magia/kernel.h>
#include <magia/klog.h>
#include <magia/linker_set.h>
#include <magia/spinlock.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

#define COM1 0x3f8

static spinlock_t con_lock = SPINLOCK_INIT("console");
static bool serial_ok;

static void serial_putc(char c)
{
	if (!serial_ok)
		return;
	for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++)
		cpu_relax();
	outb(COM1, (uint8_t)c);
}

static void putc_locked(char c)
{
	if (c == '\n')
		serial_putc('\r');
	serial_putc(c);
	fbcon_putc(c);
}

void console_init(void)
{
	outb(COM1 + 1, 0x00); /* no interrupts yet */
	outb(COM1 + 3, 0x80); /* DLAB */
	outb(COM1 + 0, 0x01); /* 115200 baud */
	outb(COM1 + 1, 0x00);
	outb(COM1 + 3, 0x03); /* 8N1 */
	outb(COM1 + 2, 0xc7); /* FIFO on, clear, 14-byte threshold */
	outb(COM1 + 4, 0x0b); /* DTR, RTS, OUT2 (IRQ line enable) */
	serial_ok = inb(COM1 + 5) != 0xff;
	fbcon_init();
}

static void emit(const char *s, size_t n, bool log)
{
	if (panicking) { /* the lock may be held by the code that panicked */
		if (log)
			klog_put(&kernel_log, s, n);
		for (size_t i = 0; i < n; i++)
			putc_locked(s[i]);
		fbcon_flush();
		return;
	}
	spin_lock(&con_lock);
	if (log)
		klog_put(&kernel_log, s, n);
	for (size_t i = 0; i < n; i++)
		putc_locked(s[i]);
	fbcon_flush();
	spin_unlock(&con_lock);
}

void console_write(const char *s, size_t n) { emit(s, n, true); }

/* dmesg: print log bytes without logging them a second time. */
void console_write_nolog(const char *s, size_t n) { emit(s, n, false); }

/* Run fn with the console lock held (it protects the framebuffer
 * console's state), e.g. to scroll or to draw the panic screen. */
void console_locked(void (*fn)(void *), void *arg)
{
	if (panicking) {
		fn(arg);
		return;
	}
	spin_lock(&con_lock);
	fn(arg);
	spin_unlock(&con_lock);
}

/* VT220 keys from every input source (serial terminals, and the PS/2 and
 * USB keyboards, which send the same sequences): ESC [ n ~ with n = 5
 * (Page Up) or 6 (Page Down) become VT_PGUP / VT_PGDN; other escape
 * sequences are swallowed; everything else passes through. */
int vt_decode(struct vt_dec *d, int c)
{
	switch (d->state) {
	case 0:
		if (c == 0x1b) {
			d->state = 1;
			return -1;
		}
		return c;
	case 1:
		d->state = c == '[' ? 2 : 0;
		d->num = 0;
		return -1;
	default:
		if (c >= '0' && c <= '9') {
			if (d->num < 1000)
				d->num = d->num * 10 + (c - '0');
			return -1;
		}
		d->state = 0;
		if (c == '~' && d->num == 5)
			return VT_PGUP;
		if (c == '~' && d->num == 6)
			return VT_PGDN;
		return -1; /* arrows (ESC [ A) and the rest: not used yet */
	}
}

void console_putc(char c) { console_write(&c, 1); }

/* ---------------- input ---------------- */

#define RX_SIZE 256
static volatile uint8_t rx_buf[RX_SIZE];
static volatile uint32_t rx_head, rx_tail;
static volatile int rx_waiter = -1;
volatile uint64_t console_rx_bytes;

/* Called from IRQ handlers (COM1, PS/2 keyboard) with interrupts off. */
void console_rx_push(uint8_t c)
{
	console_rx_bytes++;
	if (rx_head - rx_tail < RX_SIZE)
		rx_buf[rx_head++ % RX_SIZE] = c;
	if (rx_waiter >= 0)
		thread_wake(rx_waiter);
}

static void com1_irq(struct trapframe *tf)
{
	(void)tf;
	while (inb(COM1 + 5) & 0x01)
		console_rx_push(inb(COM1));
}

void console_input_init(void)
{
	irq_register(IRQ_COM1, com1_irq);
	outb(COM1 + 1, 0x01); /* interrupt on received data */
	irq_enable(IRQ_COM1);
}

int console_getc(void)
{
	uint64_t f = irq_save();
	int c = -1;
	if (rx_tail != rx_head)
		c = rx_buf[rx_tail++ % RX_SIZE];
	irq_restore(f);
	return c;
}

int console_getc_wait(void)
{
	for (;;) {
		sleep_prepare(0);
		rx_waiter = curthread()->tid;
		int c = console_getc();
		if (c >= 0) {
			rx_waiter = -1;
			sleep_cancel();
			return c;
		}
		sleep_commit();
	}
}

static void console_input_sysinit(void) { console_input_init(); }
SYSINIT(console_input, 40002, console_input_sysinit); /* after interrupts (SI_ORDER_INTR) */
