/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * console.c - COM1 (16550 UART) and VGA text console.
 *
 * Output goes to both devices; serial is the primary console (QEMU
 * -nographic, the test harness and the magia> monitor all use it).
 * Input arrives on IRQ4 into a small ring buffer; console_getc_wait()
 * sleeps until a byte is available.
 */
#include <magia/kernel.h>
#include <magia/spinlock.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

#define COM1 0x3f8
#define VGA_COLS 80
#define VGA_ROWS 25

static spinlock_t con_lock = SPINLOCK_INIT("console");
static uint16_t *vga;
static int vga_x, vga_y;
static bool serial_ok;

static void serial_putc(char c)
{
	if (!serial_ok)
		return;
	for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++)
		cpu_relax();
	outb(COM1, (uint8_t)c);
}

static void vga_scroll(void)
{
	memmove(vga, vga + VGA_COLS, (VGA_ROWS - 1) * VGA_COLS * 2);
	for (int x = 0; x < VGA_COLS; x++)
		vga[(VGA_ROWS - 1) * VGA_COLS + x] = 0x0700 | ' ';
	vga_y = VGA_ROWS - 1;
}

static void vga_putc(char c)
{
	if (!vga)
		return;
	if (c == '\n') {
		vga_x = 0;
		if (++vga_y >= VGA_ROWS)
			vga_scroll();
		return;
	}
	if (c == '\r')
		return;
	if (c == '\b') {
		if (vga_x > 0)
			vga_x--;
		return;
	}
	vga[vga_y * VGA_COLS + vga_x] = 0x0700 | (uint8_t)c;
	if (++vga_x >= VGA_COLS) {
		vga_x = 0;
		if (++vga_y >= VGA_ROWS)
			vga_scroll();
	}
}

static void putc_locked(char c)
{
	if (c == '\n')
		serial_putc('\r');
	serial_putc(c);
	vga_putc(c);
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

	vga = phys_to_virt(0xb8000);
	for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
		vga[i] = 0x0700 | ' ';
	vga_x = vga_y = 0;
}

void console_write(const char *s, size_t n)
{
	if (panicking) {
		for (size_t i = 0; i < n; i++)
			putc_locked(s[i]);
		return;
	}
	spin_lock(&con_lock);
	for (size_t i = 0; i < n; i++)
		putc_locked(s[i]);
	spin_unlock(&con_lock);
}

void console_putc(char c) { console_write(&c, 1); }

/* ---------------- input ---------------- */

#define RX_SIZE 256
static volatile uint8_t rx_buf[RX_SIZE];
static volatile uint32_t rx_head, rx_tail;
static volatile int rx_waiter = -1;
volatile uint64_t console_rx_bytes;

static void com1_irq(struct trapframe *tf)
{
	(void)tf;
	while (inb(COM1 + 5) & 0x01) {
		uint8_t c = inb(COM1);
		console_rx_bytes++;
		if (rx_head - rx_tail < RX_SIZE)
			rx_buf[rx_head++ % RX_SIZE] = c;
	}
	if (rx_waiter >= 0)
		thread_wake(rx_waiter);
}

void console_input_init(void)
{
	irq_register(IRQ_COM1, com1_irq);
	outb(COM1 + 1, 0x01); /* interrupt on received data */
	pic_unmask(IRQ_COM1);
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
