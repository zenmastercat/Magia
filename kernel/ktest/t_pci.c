/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* t_pci.c - ktests for PCI/PCIe enumeration, BARs and MSI-X (06x). */
#include <magia/ktest.h>
#include <magia/apic.h>
#include <magia/mm.h>
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

/* QEMU's xHCI: BAR0 is a 64-bit memory BAR of 16 KiB, naturally aligned,
 * and sizing it left the device decoding as before. */
KTEST(061, pci_bars_are_decoded_and_sized)
{
	struct pci_dev *x = pci_find_class(0x0c, 0x03, 0x30, 0);
	KT_ASSERT(x != NULL);
	if (!x)
		return;
	const struct pci_bar *b = &x->bar[0];
	KT_ASSERT(!b->io && b->is64);
	KT_EQ(b->size, 0x4000);
	KT_ASSERT(b->base != 0);
	KT_EQ(b->base & (b->size - 1), 0);
	KT_EQ(x->bar[1].size, 0); /* the high half of BAR0 */
	KT_ASSERT(pci_read16(x, PCI_COMMAND) & PCI_CMD_MEM);
	uint32_t lo = pci_read32(x, PCI_BAR0);
	KT_EQ(lo & ~0xfu, (uint32_t)b->base); /* restored after sizing */
	uint8_t ids[16];
	int n = pci_cap_walk(x, ids, 16);
	bool msix = false;
	for (int i = 0; i < n && i < 16; i++)
		msix |= ids[i] == PCI_CAP_MSIX;
	KT_ASSERT(msix);
	KT_ASSERT(pci_find_cap(x, PCI_CAP_MSIX) >= 0x40);
}

/* A function whose config space is a RAM buffer we control: malformed
 * capability lists must stop the walk instead of looping or wandering. */
KTEST(062, pci_capability_walk_rejects_garbage)
{
	uint8_t *cfg = kzalloc(4096);
	struct pci_dev fake = { .cfg = cfg };
	cfg[PCI_STATUS] = PCI_STATUS_CAPS;
	uint8_t ids[64];
	/* a loop: 0x40 -> 0x50 -> 0x40 */
	cfg[PCI_CAP_PTR] = 0x40;
	cfg[0x40] = 0x09, cfg[0x41] = 0x50;
	cfg[0x50] = 0x05, cfg[0x51] = 0x40;
	KT_EQ(pci_cap_walk(&fake, ids, 64), 2);
	KT_EQ(pci_find_cap(&fake, PCI_CAP_MSIX), 0);
	KT_EQ(pci_find_cap(&fake, PCI_CAP_MSI), 0x50);
	/* pointers into the header, unaligned, or self-referencing */
	cfg[PCI_CAP_PTR] = 0x20;
	KT_EQ(pci_cap_walk(&fake, ids, 64), 0);
	cfg[PCI_CAP_PTR] = 0x42;
	KT_EQ(pci_cap_walk(&fake, ids, 64), 0);
	cfg[PCI_CAP_PTR] = 0x40, cfg[0x41] = 0x40;
	KT_EQ(pci_cap_walk(&fake, ids, 64), 1);
	/* a long chain through all 48 dword slots stops at 48 */
	cfg[PCI_CAP_PTR] = 0x40;
	for (int p = 0x40; p < 0x100; p += 4)
		cfg[p] = 0x01, cfg[p + 1] = (uint8_t)(p + 4 < 0x100 ? p + 4 : 0);
	KT_EQ(pci_cap_walk(&fake, ids, 64), 48);
	/* no capabilities bit: nothing */
	cfg[PCI_STATUS] = 0;
	KT_EQ(pci_cap_walk(&fake, ids, 64), 0);
	/* an MSI-X table outside its BAR is refused, and MSI is the fallback */
	cfg[PCI_STATUS] = PCI_STATUS_CAPS;
	memset(cfg + 0x40, 0, 0xc0);
	cfg[PCI_CAP_PTR] = 0x40;
	cfg[0x40] = PCI_CAP_MSIX, cfg[0x41] = 0;
	cfg[0x42] = 0x07;                        /* 8 entries */
	*(uint32_t *)(cfg + 0x44) = 0x1000 | 0;  /* BAR0 + 4 KiB */
	fake.bar[0] = (struct pci_bar){ .base = 0xfeb00000, .size = 0x1000 };
	bool mx = true;
	KT_ASSERT(pci_enable_msi(&fake, NULL, &mx) < 0);
	KT_EQ(fake.msi_vec, 0);
	KT_ASSERT(pci_enable_msi(&fake, NULL, &mx) < 0); /* and again: no vector leaks */
	kfree(cfg);
}

static void test_msi(struct trapframe *tf) { (void)tf; }

/* MSI-X on the xHCI: entry 0 points at the BSP with an MSI vector, the
 * function is enabled and unmasked, INTx is off. Delivery: xhci.c. */
KTEST(063, pci_msix_programmed)
{
	struct pci_dev *x = pci_find_class(0x0c, 0x03, 0x30, 0);
	KT_ASSERT(x != NULL);
	if (!x)
		return;
	bool msix = false;
	int vec = x->msi_vec ? x->msi_vec : pci_enable_msi(x, test_msi, &msix);
	KT_ASSERT(vec >= VEC_MSI_FIRST && vec <= VEC_MSI_LAST);
	KT_ASSERT(x->msix);
	int cap = pci_find_cap(x, PCI_CAP_MSIX);
	uint16_t ctrl = pci_read16(x, (uint32_t)cap + 2);
	KT_EQ(ctrl & 0xc000, 0x8000); /* enabled, function not masked */
	uint32_t tbl = pci_read32(x, (uint32_t)cap + 4);
	volatile uint32_t *t = phys_to_virt(x->bar[tbl & 7].base + (tbl & ~7u));
	KT_EQ(t[0] & 0xfff00000u, 0xfee00000u);
	KT_EQ((t[0] >> 12) & 0xff, lapic_bsp_id & 0xff);
	KT_EQ(t[2] & 0xff, vec);
	KT_EQ(t[3] & 1, 0);
	KT_ASSERT(pci_read16(x, PCI_COMMAND) & PCI_CMD_INTX_OFF);
	KT_ASSERT(pci_read16(x, PCI_COMMAND) & PCI_CMD_MASTER);
}
