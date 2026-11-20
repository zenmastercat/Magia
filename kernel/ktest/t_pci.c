/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* t_pci.c - ktests for PCI/PCIe enumeration, BARs and MSI-X (06x). */
#include <magia/ktest.h>
#include <magia/pci.h>

static struct pci_dev *find(uint16_t vendor, uint16_t device)
{
	for (int i = 0; i < pci_ndevs; i++)
		if (pci_devs[i].vendor == vendor && pci_devs[i].device == device)
			return &pci_devs[i];
	return NULL;
}

/* QEMU q35 with -device qemu-xhci: what the ECAM scan must find. */
KTEST(060, pci_enumerates_q35)
{
	KT_ASSERT(pci_ndevs >= 4);
	struct pci_dev *host = find(0x8086, 0x29c0), *lpc = find(0x8086, 0x2918);
	struct pci_dev *xhci = pci_find_class(0x0c, 0x03, 0x30, 0);
	KT_ASSERT(host && host->bus == 0 && host->dev == 0 && host->fn == 0);
	KT_ASSERT(host && host->class == 0x06 && host->subclass == 0x00);
	KT_ASSERT(lpc && lpc->dev == 0x1f && lpc->class == 0x06 && lpc->subclass == 0x01);
	KT_ASSERT(xhci && xhci->vendor == 0x1b36 && xhci->device == 0x000d);
	/* config reads are bounded to the function's 4 KiB window */
	KT_EQ(pci_read32(host, 4096), 0xffffffffu);
	KT_EQ(pci_read32(host, 4094), 0xffffffffu);
	KT_EQ(pci_read16(host, 3), 0xffff);       /* misaligned */
	KT_EQ(pci_read8(NULL, 0), 0xff);
	KT_EQ(pci_read16(host, PCI_VENDOR), 0x8086);
	KT_ASSERT(pci_find_class(0x0c, 0x03, 0x30, 1) == NULL); /* only one xHCI */
	KT_ASSERT(strcmp(pci_class_name(0x0c, 0x03), "USB controller") == 0);
	/* each function appears once */
	int dups = 0;
	for (int i = 0; i < pci_ndevs; i++)
		for (int j = i + 1; j < pci_ndevs; j++)
			dups += pci_devs[i].bus == pci_devs[j].bus && pci_devs[i].dev == pci_devs[j].dev &&
				pci_devs[i].fn == pci_devs[j].fn;
	KT_EQ(dups, 0);
}
