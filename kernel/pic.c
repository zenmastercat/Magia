/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* pic.c - the legacy 8259A pair, remapped to vectors 32..47. */
#include <magia/trap.h>
#include <magia/x86.h>

#define PIC1 0x20
#define PIC2 0xa0

static uint16_t irq_mask = 0xffff & ~(1 << 2); /* cascade line stays open */

static void pic_write_mask(void)
{
	outb(PIC1 + 1, irq_mask & 0xff);
	outb(PIC2 + 1, irq_mask >> 8);
}

void pic_init(void)
{
	outb(PIC1, 0x11); /* ICW1: edge, cascade, ICW4 */
	io_wait();
	outb(PIC2, 0x11);
	io_wait();
	outb(PIC1 + 1, IRQ_BASE); /* ICW2: vector offsets */
	io_wait();
	outb(PIC2 + 1, IRQ_BASE + 8);
	io_wait();
	outb(PIC1 + 1, 4); /* ICW3: slave on IRQ2 */
	io_wait();
	outb(PIC2 + 1, 2);
	io_wait();
	outb(PIC1 + 1, 0x01); /* ICW4: 8086 mode */
	io_wait();
	outb(PIC2 + 1, 0x01);
	io_wait();
	pic_write_mask();
}

void pic_eoi(int irq)
{
	if (irq >= 8)
		outb(PIC2, 0x20);
	outb(PIC1, 0x20);
}

void pic_unmask(int irq)
{
	uint64_t f = irq_save();
	irq_mask &= ~(1u << irq);
	pic_write_mask();
	irq_restore(f);
}

void pic_mask_all(void)
{
	uint64_t f = irq_save();
	irq_mask = 0xffff;
	pic_write_mask();
	irq_restore(f);
}

void pic_mask(int irq)
{
	uint64_t f = irq_save();
	irq_mask |= 1u << irq;
	pic_write_mask();
	irq_restore(f);
}

/* Read the in-service register: a spurious IRQ 7/15 is not in service. */
bool pic_spurious(int irq)
{
	uint16_t port = irq == 7 ? PIC1 : PIC2;
	outb(port, 0x0b);
	uint8_t isr = inb(port);
	if (isr & 0x80)
		return false;
	if (irq == 15)
		outb(PIC1, 0x20); /* the master still saw the cascade IRQ */
	return true;
}
