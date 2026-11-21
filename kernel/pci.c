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
 * recursion depth is bounded, capability lists are walked with loop and
 * range checks, and an MSI-X table must lie inside its BAR.
 * Interrupts: MSI-X (or MSI) only - legacy INTx routing would need the
 * ACPI _PRT methods (AML), which Magia does not interpret.
 */
#include <magia/acpi.h>
#include <magia/linker_set.h>
#include <magia/mm.h>
#include <magia/abi.h>
#include <magia/apic.h>
#include <magia/pci.h>
#include <magia/spinlock.h>
#include <magia/trap.h>
#include <magia/x86.h>

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
void pci_read_bars(struct pci_dev *d);

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
	/* No IOMMU: a function's DMA stays off until a driver needs it
	 * (xhci_start, pci_enable_msi). Bridges keep it: it only forwards their
	 * children's DMA, and a driven child may sit behind one.
	 * shortcut: display controllers keep it too (an integrated GPU may scan
	 * the boot framebuffer out of RAM); VT-d DMA remapping (stage 4) fences them. */
	if ((tmp.header_type & 0x7f) == 0 && tmp.class != 0x03)
		pci_write16(&tmp, PCI_COMMAND, pci_read16(&tmp, PCI_COMMAND) & ~PCI_CMD_MASTER);
	if (pci_ndevs < PCI_MAX_DEVS) {
		uint64_t f = irq_save(); /* decoding is briefly off: nothing may print to the framebuffer */
		pci_read_bars(&tmp);
		irq_restore(f);
		pci_devs[pci_ndevs++] = tmp;
	}
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

/* ---------------------------------------------------------------- BARs */

/* Decode and size the BARs (PCI 3.0 6.2.5.1): write all ones, read the
 * mask back, restore; memory and I/O decoding are off meanwhile so the
 * device never answers at the temporary all-ones address. */
void pci_read_bars(struct pci_dev *d)
{
	int nbars = (d->header_type & 0x7f) == 0 ? 6 : (d->header_type & 0x7f) == 1 ? 2 : 0;
	uint16_t cmd = pci_read16(d, PCI_COMMAND);
	pci_write16(d, PCI_COMMAND, cmd & ~(PCI_CMD_IO | PCI_CMD_MEM));
	memset(d->bar, 0, sizeof(d->bar));
	for (int i = 0; i < nbars; i++) {
		uint32_t off = PCI_BAR0 + 4 * (uint32_t)i;
		uint32_t lo = pci_read32(d, off);
		struct pci_bar *b = &d->bar[i];
		pci_write32(d, off, 0xffffffffu);
		uint32_t mlo = pci_read32(d, off);
		pci_write32(d, off, lo);
		if (lo & 1) { /* I/O space */
			b->io = true;
			b->base = lo & ~3u;
			uint32_t m = mlo & ~3u & 0xffff;
			b->size = m ? (~m & 0xffff) + 1 : 0;
			continue;
		}
		b->prefetch = (lo >> 3) & 1;
		uint64_t base = lo & ~0xfu, mask = (uint64_t)(mlo & ~0xfu) | 0xffffffff00000000ULL;
		if (((lo >> 1) & 3) == 2 && i + 1 < nbars) { /* 64-bit: the next BAR is the high half */
			uint32_t hi = pci_read32(d, off + 4);
			pci_write32(d, off + 4, 0xffffffffu);
			uint32_t mhi = pci_read32(d, off + 4);
			pci_write32(d, off + 4, hi);
			b->is64 = true;
			base |= (uint64_t)hi << 32;
			mask = ((uint64_t)mhi << 32) | (mlo & ~0xfu);
			i++;
		}
		b->size = (mlo & ~0xfu) || b->is64 ? ~mask + 1 : 0;
		b->base = b->size ? base : 0;
	}
	pci_write16(d, PCI_COMMAND, cmd);
}

/* ---------------------------------------------------------------- capabilities */

/* Walk the capability list: pointers must be dword-aligned, inside the
 * device-specific area (0x40..0xff), not revisited, at most 48 entries
 * (PCI 3.0 6.7). Stores up to `max` ids; returns how many were found. */
int pci_cap_walk(const struct pci_dev *d, uint8_t ids[], int max)
{
	uint64_t seen = 0;
	int n = 0;
	if (!(pci_read16(d, PCI_STATUS) & PCI_STATUS_CAPS))
		return 0;
	uint8_t p = pci_read8(d, PCI_CAP_PTR);
	for (int i = 0; i < 48 && p >= 0x40 && !(p & 3); i++) {
		if (seen & (1ULL << (p / 4 - 16)))
			break; /* loop */
		seen |= 1ULL << (p / 4 - 16);
		if (n < max)
			ids[n] = pci_read8(d, p);
		n++;
		p = pci_read8(d, p + 1u);
	}
	return n;
}

int pci_find_cap(const struct pci_dev *d, uint8_t id)
{
	uint64_t seen = 0;
	if (!(pci_read16(d, PCI_STATUS) & PCI_STATUS_CAPS))
		return 0;
	uint8_t p = pci_read8(d, PCI_CAP_PTR);
	for (int i = 0; i < 48 && p >= 0x40 && !(p & 3); i++) {
		if (seen & (1ULL << (p / 4 - 16)))
			return 0;
		seen |= 1ULL << (p / 4 - 16);
		if (pci_read8(d, p) == id)
			return p;
		p = pci_read8(d, p + 1u);
	}
	return 0;
}

/* ---------------------------------------------------------------- MSI / MSI-X */

#define MSI_ADDR_BASE 0xfee00000u /* x86 MSI address window (SDM 11.11.1) */
static int next_vec = VEC_MSI_FIRST;
static spinlock_t msi_lock = SPINLOCK_INIT("msi");

static uint32_t msi_addr(void)
{
	/* shortcut: physical destination = BSP, 8-bit id (x2APIC ids < 256);
	 * larger ids need interrupt remapping (VT-d), stage 4. */
	return MSI_ADDR_BASE | ((lapic_bsp_id & 0xff) << 12);
}

static int setup_msix(struct pci_dev *d, int cap, int vec)
{
	uint16_t ctrl = pci_read16(d, (uint32_t)cap + 2);
	uint32_t tbl = pci_read32(d, (uint32_t)cap + 4);
	uint32_t nent = (ctrl & 0x7ff) + 1, bir = tbl & 7, off = tbl & ~7u;
	if (bir > 5)
		return -EINVAL;
	const struct pci_bar *b = &d->bar[bir];
	if (b->io || !b->base || (uint64_t)off + 16ULL * nent > b->size)
		return -EINVAL; /* the table must lie inside a memory BAR */
	volatile uint32_t *t = vmm_map_mmio(b->base + off, 16ULL * nent);
	if (!t)
		return -EINVAL;
	pci_write16(d, (uint32_t)cap + 2, ctrl | 0x4000); /* function mask while programming */
	for (uint32_t i = 0; i < nent; i++)
		t[i * 4 + 3] |= 1; /* mask every entry */
	t[0] = msi_addr();
	t[1] = 0;
	t[2] = (uint32_t)vec; /* fixed delivery, edge */
	t[3] &= ~1u;          /* unmask entry 0 */
	pci_write16(d, (uint32_t)cap + 2, (uint16_t)((ctrl | 0x8000) & ~0x4000)); /* enable, unmask */
	return 0;
}

static int setup_msi(struct pci_dev *d, int cap, int vec)
{
	uint16_t ctrl = pci_read16(d, (uint32_t)cap + 2);
	bool is64 = ctrl & 0x80;
	pci_write32(d, (uint32_t)cap + 4, msi_addr());
	if (is64)
		pci_write32(d, (uint32_t)cap + 8, 0);
	pci_write16(d, (uint32_t)cap + (is64 ? 12 : 8), (uint16_t)vec);
	pci_write16(d, (uint32_t)cap + 2, (uint16_t)((ctrl & ~0x70) | 1)); /* one vector, enabled */
	return 0;
}

int pci_enable_msi(struct pci_dev *d, void (*handler)(struct trapframe *), bool *msix)
{
	if (d->msi_vec) { /* already set up: just (re)attach the handler */
		irq_register_vector(d->msi_vec, handler);
		*msix = d->msix;
		return d->msi_vec;
	}
	int cx = pci_find_cap(d, PCI_CAP_MSIX), cm = pci_find_cap(d, PCI_CAP_MSI);
	if (!cx && !cm)
		return -ENOENT;
	spin_lock(&msi_lock);
	int vec = next_vec <= VEC_MSI_LAST ? next_vec++ : -1;
	spin_unlock(&msi_lock);
	if (vec < 0)
		return -EBUSY;
	irq_register_vector(vec, handler);
	int r = cx ? setup_msix(d, cx, vec) : setup_msi(d, cm, vec);
	if (r < 0 && cx && cm)
		r = setup_msi(d, cm, vec); /* MSI-X table unusable: fall back to MSI */
	if (r < 0) {
		irq_register_vector(vec, NULL);
		spin_lock(&msi_lock);
		if (vec == next_vec - 1)
			next_vec--; /* give the vector back */
		spin_unlock(&msi_lock);
		return r;
	}
	d->msix = cx && r == 0 && (pci_read16(d, (uint32_t)cx + 2) & 0x8000);
	d->msi_vec = vec;
	pci_write16(d, PCI_COMMAND, pci_read16(d, PCI_COMMAND) | PCI_CMD_INTX_OFF | PCI_CMD_MASTER);
	*msix = d->msix;
	return vec;
}
