/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* pci.h - PCI/PCIe configuration space over ECAM, BARs, MSI/MSI-X. */
#ifndef MAGIA_PCI_H
#define MAGIA_PCI_H

#include <magia/kernel.h>

/* Configuration space offsets (PCI Local Bus 3.0, type 0 and 1 headers) */
#define PCI_VENDOR      0x00
#define PCI_DEVICE      0x02
#define PCI_COMMAND     0x04
#define PCI_STATUS      0x06
#define PCI_REVISION    0x08
#define PCI_PROG_IF     0x09
#define PCI_SUBCLASS    0x0a
#define PCI_CLASS       0x0b
#define PCI_HEADER_TYPE 0x0e
#define PCI_BAR0        0x10
#define PCI_SECONDARY_BUS 0x19 /* type 1 (bridge) */
#define PCI_CAP_PTR     0x34
#define PCI_INTR_LINE   0x3c

#define PCI_CMD_IO      0x0001
#define PCI_CMD_MEM     0x0002
#define PCI_CMD_MASTER  0x0004
#define PCI_CMD_INTX_OFF 0x0400
#define PCI_STATUS_CAPS 0x0010

#define PCI_CAP_MSI  0x05
#define PCI_CAP_MSIX 0x11

#define PCI_MAX_DEVS 128

struct trapframe;

struct pci_bar {
	uint64_t base;   /* physical address (memory) or port (I/O); 0 if unused */
	uint64_t size;
	bool io, is64, prefetch;
};

struct pci_dev {
	uint8_t bus, dev, fn;
	uint8_t class, subclass, prog_if, revision, header_type;
	uint16_t vendor, device;
	struct pci_bar bar[6];
	volatile uint8_t *cfg; /* this function's 4 KiB ECAM window */
};

extern struct pci_dev pci_devs[PCI_MAX_DEVS];
extern int pci_ndevs;

void pci_init(void);
uint8_t pci_read8(const struct pci_dev *d, uint32_t off);   /* 0xff.. if off is bad */
uint16_t pci_read16(const struct pci_dev *d, uint32_t off);
uint32_t pci_read32(const struct pci_dev *d, uint32_t off);
void pci_write16(const struct pci_dev *d, uint32_t off, uint16_t v);
void pci_write32(const struct pci_dev *d, uint32_t off, uint32_t v);
struct pci_dev *pci_find_class(uint8_t class, uint8_t subclass, uint8_t prog_if, int index);
const char *pci_class_name(uint8_t class, uint8_t subclass);
void pci_dump(void); /* lspci */

/* BARs and capabilities */
void pci_read_bars(struct pci_dev *d);
int pci_find_cap(const struct pci_dev *d, uint8_t id); /* offset or 0 */
int pci_cap_walk(const struct pci_dev *d, uint8_t ids[], int max); /* number found */

/* MSI-X (preferred) or MSI: one vector to the BSP. Returns the IDT vector
 * or a negative errno; msix tells which one was used. */
int pci_enable_msi(struct pci_dev *d, void (*handler)(struct trapframe *), bool *msix);

#endif
