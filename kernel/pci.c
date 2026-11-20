/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * pci.c - PCI/PCIe enumeration over ECAM (the memory-mapped configuration
 * space whose base the ACPI MCFG table gives).
 *
 * Written from the PCI Local Bus 3.0 and PCIe 4.0 specifications; FreeBSD
 * sys/dev/pci is the design model (scan bus 0, follow bridges to their
 * secondary buses). Function f of device d on bus b has its 4 KiB of
 * configuration space at base + (b << 20 | d << 15 | f << 12); each bus's
 * 1 MiB is mapped uncached the first time it is scanned. Segment 0 only.
 * Everything read from config space is device input: offsets are checked
 * against the 4 KiB window, the bridge walk visits each bus once and
 * recursion depth is bounded.
 */
#include <magia/acpi.h>
#include <magia/linker_set.h>
#include <magia/mm.h>
#include <magia/pci.h>

struct pci_dev pci_devs[PCI_MAX_DEVS];
int pci_ndevs;
static uint64_t ecam_base;
static uint8_t bus_lo, bus_hi;
static volatile uint8_t *bus_va[256];
static uint64_t visited[4]; /* one bit per bus */

static volatile uint8_t *bus_window(uint8_t bus)
{
	if (bus < bus_lo || bus > bus_hi)
		return NULL;
	if (!bus_va[bus])
		bus_va[bus] = vmm_map_mmio(ecam_base + ((uint64_t)bus << 20), 1 << 20);
	return bus_va[bus];
}

static bool off_ok(const struct pci_dev *d, uint32_t off, uint32_t size)
{
	return d && d->cfg && off < 4096 && off + size <= 4096 && (off & (size - 1)) == 0;
}

uint8_t pci_read8(const struct pci_dev *d, uint32_t off)
{
	return off_ok(d, off, 1) ? d->cfg[off] : 0xff;
}

uint16_t pci_read16(const struct pci_dev *d, uint32_t off)
{
	return off_ok(d, off, 2) ? *(volatile uint16_t *)(d->cfg + off) : 0xffff;
}

uint32_t pci_read32(const struct pci_dev *d, uint32_t off)
{
	return off_ok(d, off, 4) ? *(volatile uint32_t *)(d->cfg + off) : 0xffffffffu;
}

void pci_write16(const struct pci_dev *d, uint32_t off, uint16_t v)
{
	if (off_ok(d, off, 2))
		*(volatile uint16_t *)(d->cfg + off) = v;
}

void pci_write32(const struct pci_dev *d, uint32_t off, uint32_t v)
{
	if (off_ok(d, off, 4))
		*(volatile uint32_t *)(d->cfg + off) = v;
}

static void scan_bus(uint8_t bus, int depth);

static void scan_function(volatile uint8_t *win, uint8_t bus, uint8_t dev, uint8_t fn, int depth)
{
	struct pci_dev tmp = { .bus = bus, .dev = dev, .fn = fn, .cfg = win + ((uint32_t)dev << 15 | (uint32_t)fn << 12) };
	tmp.vendor = pci_read16(&tmp, PCI_VENDOR);
	if (tmp.vendor == 0xffff || tmp.vendor == 0)
		return;
	tmp.device = pci_read16(&tmp, PCI_DEVICE);
	tmp.revision = pci_read8(&tmp, PCI_REVISION);
	tmp.prog_if = pci_read8(&tmp, PCI_PROG_IF);
	tmp.subclass = pci_read8(&tmp, PCI_SUBCLASS);
	tmp.class = pci_read8(&tmp, PCI_CLASS);
	tmp.header_type = pci_read8(&tmp, PCI_HEADER_TYPE);
	if (pci_ndevs < PCI_MAX_DEVS)
		pci_devs[pci_ndevs++] = tmp;
	/* shortcut: trusts the firmware's bus numbering (no re-assignment),
	 * enough for UEFI-booted PCs; hot-plug bridges would need resources. */
	if ((tmp.header_type & 0x7f) == 1 && depth < 16) {
		uint8_t sec = pci_read8(&tmp, PCI_SECONDARY_BUS);
		if (sec > bus)
			scan_bus(sec, depth + 1);
	}
}

static void scan_bus(uint8_t bus, int depth)
{
	if (visited[bus / 64] & (1ULL << (bus % 64)))
		return;
	visited[bus / 64] |= 1ULL << (bus % 64);
	volatile uint8_t *win = bus_window(bus);
	if (!win)
		return;
	for (uint8_t dev = 0; dev < 32; dev++) {
		volatile uint8_t *f0 = win + ((uint32_t)dev << 15);
		if (*(volatile uint16_t *)f0 == 0xffff)
			continue;
		int nfn = (f0[PCI_HEADER_TYPE] & 0x80) ? 8 : 1;
		for (uint8_t fn = 0; fn < nfn; fn++)
			scan_function(win, bus, dev, fn, depth);
	}
}

struct pci_dev *pci_find_class(uint8_t class, uint8_t subclass, uint8_t prog_if, int index)
{
	for (int i = 0; i < pci_ndevs; i++) {
		struct pci_dev *d = &pci_devs[i];
		if (d->class == class && d->subclass == subclass && d->prog_if == prog_if && index-- == 0)
			return d;
	}
	return NULL;
}

const char *pci_class_name(uint8_t class, uint8_t subclass)
{
	static const struct {
		uint8_t c, s;
		const char *name;
	} names[] = {
		{ 0x01, 0x01, "IDE controller" }, { 0x01, 0x06, "SATA controller" },
		{ 0x01, 0x08, "NVMe controller" }, { 0x02, 0x00, "Ethernet controller" },
		{ 0x03, 0x00, "VGA controller" }, { 0x03, 0x80, "display controller" },
		{ 0x04, 0x03, "audio device" }, { 0x06, 0x00, "host bridge" },
		{ 0x06, 0x01, "ISA bridge" }, { 0x06, 0x04, "PCI bridge" },
		{ 0x0c, 0x03, "USB controller" }, { 0x0c, 0x05, "SMBus controller" },
	};
	for (size_t i = 0; i < ARRAY_SIZE(names); i++)
		if (names[i].c == class && names[i].s == subclass)
			return names[i].name;
	return "device";
}

void pci_dump(void)
{
	for (int i = 0; i < pci_ndevs; i++) {
		const struct pci_dev *d = &pci_devs[i];
		kprintf("%02x:%02x.%u %04x:%04x class %02x/%02x/%02x rev %02x %s\n", d->bus, d->dev, d->fn,
			d->vendor, d->device, d->class, d->subclass, d->prog_if, d->revision,
			pci_class_name(d->class, d->subclass));
	}
}

void pci_init(void)
{
	if (!(acpi.found & ACPI_FOUND_MCFG) || acpi.nmcfg == 0) {
		kprintf("pci: no MCFG table, no PCIe configuration access\n");
		return;
	}
	ecam_base = acpi.mcfg_base[0]; /* shortcut: segment 0 only, enough for desktop PCs */
	bus_lo = acpi.mcfg_start_bus[0];
	bus_hi = acpi.mcfg_end_bus[0];
	scan_bus(bus_lo, 0);
	kprintf("pci: ECAM %p buses %u-%u, %d functions\n", (void *)ecam_base, bus_lo, bus_hi, pci_ndevs);
}

static void pci_sysinit(void) { pci_init(); }
SYSINIT(pci, 40003, pci_sysinit); /* after interrupts and console input */
