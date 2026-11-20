/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * vmm.c - 4-level page tables with W^X.
 *
 * Replaces Limine's boot page tables with:
 *   .text    RX          (4 KiB pages, global)
 *   .rodata  R  + NX
 *   .data/.bss RW + NX
 *   direct map at dmap_base (Limine's randomised HHDM offset) of every RAM,
 *     ACPI and firmware-data range in the memory map: RW + NX, write-back,
 *     2 MiB pages where aligned, no size cap; the frames of the kernel image
 *     itself are read-only there, so the image has no writable alias of its
 *     code. The framebuffer is mapped write-combining, MMIO uncached on
 *     demand (vmm_map_mmio); reserved ranges and holes are not mapped.
 *   nothing at virtual address 0 (NULL dereferences fault).
 * No mapping is ever both writable and executable (checked by ktest 1xx).
 * User windows (uvm.c) live below USER_TOP with the U bit.
 */
#include <magia/abi.h>
#include <magia/boot.h>
#include <magia/cpu.h>
#include <magia/linker_set.h>
#include <magia/mm.h>
#include <magia/spinlock.h>
#include <magia/x86.h>

extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[];
extern char __data_start[], __bss_end[], __kernel_start[], __kernel_end[];

bool nx_enabled;
static uint64_t *kernel_pml4;
static spinlock_t vmm_lock = SPINLOCK_INIT("vmm");
uint64_t vmm_pt_pages;

static uint64_t nx(void) { return nx_enabled ? PTE_NX : 0; }

static uint64_t *pt_alloc(void)
{
	uint64_t pa = pmm_alloc(0);
	if (!pa)
		panic("vmm: out of memory for page tables");
	memset(phys_to_virt(pa), 0, PAGE_SIZE);
	vmm_pt_pages++;
	return phys_to_virt(pa);
}

/* Return the PTE slot for `va` at level `stop` (1 = 4 KiB PTE, 2 = PDE),
 * creating intermediate tables when `create`. */
static uint64_t *walk(uint64_t *pml4, uint64_t va, int stop, bool create)
{
	uint64_t *table = pml4;
	bool user = va < USER_TOP;
	for (int level = 4; level > stop; level--) {
		int idx = (va >> (12 + 9 * (level - 1))) & 511;
		uint64_t e = table[idx];
		if (!(e & PTE_P)) {
			if (!create)
				return NULL;
			uint64_t *n = pt_alloc();
			e = virt_to_phys(n) | PTE_P | PTE_W | (user ? PTE_U : 0);
			table[idx] = e;
		} else if (e & PTE_PS) {
			return NULL; /* covered by a large page */
		}
		table = phys_to_virt(e & PTE_ADDR_MASK);
	}
	return &table[(va >> (12 + 9 * (stop - 1))) & 511];
}

/*
 * Immutable kernel image (OpenBSD mimmutable(2) idea, re-implemented):
 * once vmm_lock_kernel() ran, the mappings of the kernel window (PML4
 * slot 511: the image, text R+X, rodata R, data/bss RW+NX) can no longer
 * be changed through vmm_map/vmm_protect/vmm_unmap (-EPERM), and the
 * page-table pages that hold them are read-only in the direct map, so a
 * stray or malicious write to a PTE faults instead of remapping the
 * kernel. shortcut: the PML4 itself and the direct map's own tables stay
 * writable (user windows and MMIO still need them); a kernel-write
 * primitive could re-point PML4[511]. Upgrade with per-process PML4s
 * built from a locked template (stage 3) and CR0.WP-protected PML4 pages.
 */
static bool kernel_locked;
static uint64_t immutable_start; /* first VA of PML4 slot 511 */

static bool immutable(uint64_t va) { return kernel_locked && va >= immutable_start; }

int vmm_map(uint64_t va, uint64_t pa, uint64_t flags)
{
	if (!nx_enabled)
		flags &= ~PTE_NX;
	if ((flags & PTE_W) && !(flags & PTE_NX) && nx_enabled)
		panic("vmm_map: refusing W+X mapping at %p", (void *)va);
	if (immutable(va))
		return -EPERM;
	spin_lock(&vmm_lock);
	uint64_t *pte = walk(kernel_pml4, va, 1, true);
	if (!pte) {
		spin_unlock(&vmm_lock);
		return -EINVAL;
	}
	*pte = (pa & PTE_ADDR_MASK) | flags | PTE_P;
	invlpg(va);
	spin_unlock(&vmm_lock);
	return 0;
}

/* Change the flags of an existing 4 KiB mapping. */
int vmm_protect(uint64_t va, uint64_t flags)
{
	if (!nx_enabled)
		flags &= ~PTE_NX;
	if ((flags & PTE_W) && !(flags & PTE_NX) && nx_enabled)
		return -EINVAL;
	if (immutable(va))
		return -EPERM;
	spin_lock(&vmm_lock);
	uint64_t *pte = walk(kernel_pml4, va, 1, false);
	int r = -ENOENT;
	if (pte && (*pte & PTE_P)) {
		*pte = (*pte & PTE_ADDR_MASK) | flags | PTE_P;
		invlpg(va);
		r = 0;
	}
	spin_unlock(&vmm_lock);
	return r;
}

int vmm_unmap(uint64_t va, uint64_t *old_pa)
{
	if (immutable(va))
		return -EPERM;
	spin_lock(&vmm_lock);
	uint64_t *pte = walk(kernel_pml4, va, 1, false);
	int r = -ENOENT;
	if (pte && (*pte & PTE_P)) {
		if (old_pa)
			*old_pa = *pte & PTE_ADDR_MASK;
		*pte = 0;
		invlpg(va);
		r = 0;
	}
	spin_unlock(&vmm_lock);
	return r;
}

bool vmm_translate(uint64_t va, uint64_t *pa, uint64_t *flags)
{
	uint64_t *table = kernel_pml4 ? kernel_pml4 : phys_to_virt(read_cr3() & PTE_ADDR_MASK);
	for (int level = 4; level >= 1; level--) {
		int idx = (va >> (12 + 9 * (level - 1))) & 511;
		uint64_t e = table[idx];
		if (!(e & PTE_P))
			return false;
		if (level == 1 || (e & PTE_PS)) {
			uint64_t size = 1ULL << (12 + 9 * (level - 1));
			if (pa)
				*pa = (e & PTE_ADDR_MASK & ~(size - 1)) | (va & (size - 1));
			if (flags)
				*flags = e & ~PTE_ADDR_MASK;
			return true;
		}
		table = phys_to_virt(e & PTE_ADDR_MASK);
	}
	return false;
}

static void map_range_4k(uint64_t va, uint64_t va_end, uint64_t flags)
{
	for (; va < va_end; va += PAGE_SIZE) {
		uint64_t *pte = walk(kernel_pml4, va, 1, true);
		*pte = virt_to_phys((void *)va) | flags | PTE_P;
	}
}

#define SZ_2M (2ULL << 20)

/* Direct-map physical [s, e) at dmap_base + pa: 2 MiB pages where a whole
 * aligned 2 MiB frame lies inside the range (if big), 4 KiB pages elsewhere. */
static void dmap_range(uint64_t s, uint64_t e, uint64_t flags, bool big)
{
	s = ALIGN_DOWN(s, PAGE_SIZE);
	e = ALIGN_UP(e, PAGE_SIZE);
	while (s < e) {
		if (big && (s & (SZ_2M - 1)) == 0 && e - s >= SZ_2M) {
			uint64_t *pde = walk(kernel_pml4, dmap_base + s, 2, true);
			if (pde && !(*pde & PTE_P)) {
				*pde = s | flags | PTE_PS | PTE_P;
				s += SZ_2M;
				continue;
			}
		}
		uint64_t *pte = walk(kernel_pml4, dmap_base + s, 1, true);
		if (pte) /* NULL: already inside a 2 MiB mapping */
			*pte = s | flags | PTE_P;
		s += PAGE_SIZE;
	}
}

/* Memory the direct map covers with write-back caching: RAM, the kernel,
 * ACPI tables and the firmware data Limine maps (SMBIOS, EFI runtime).
 * Never MMIO or plain "reserved" ranges. */
static bool dmap_wb_type(uint64_t t)
{
	return t == MEM_USABLE || t == MEM_BOOT_RECLAIM || t == MEM_KERNEL || t == MEM_ACPI_RECLAIM ||
	       t == MEM_ACPI_NVS || t == MEM_RESERVED_MAPPED;
}

/* WB-map [s, e) RW, except the kernel image's own frames, which are
 * read-only there so the image has no writable alias of its code. */
static void dmap_wb(uint64_t s, uint64_t e, uint64_t kps, uint64_t kpe)
{
	uint64_t rw = PTE_W | PTE_G | nx();
	if (e <= kps || s >= kpe) {
		dmap_range(s, e, rw, true);
		return;
	}
	if (s < kps)
		dmap_range(s, kps, rw, true);
	dmap_range(MAX(s, kps), MIN(e, kpe), PTE_G | nx(), false);
	if (e > kpe)
		dmap_range(kpe, e, rw, true);
}

uint64_t vmm_dmap_bytes;

void vmm_init(void)
{
	nx_enabled = (rdmsr(MSR_EFER) & EFER_NXE) != 0;
	kernel_pml4 = pt_alloc();
	/* Pre-create the upper half PDPTs so the kernel half is identical in
	 * any future per-process PML4 (just copy entries 256..511). */
	for (int i = 256; i < 512; i++)
		kernel_pml4[i] = virt_to_phys(pt_alloc()) | PTE_P | PTE_W;

	/* kernel image, per section */
	map_range_4k((uint64_t)__text_start, (uint64_t)__text_end, PTE_G);
	map_range_4k((uint64_t)__rodata_start, (uint64_t)__rodata_end, PTE_G | nx());
	map_range_4k((uint64_t)__data_start, (uint64_t)__bss_end, PTE_G | PTE_W | nx());

	/* direct map: merge the (sorted, possibly overlapping) WB entries */
	uint64_t kps = virt_to_phys(__kernel_start);
	uint64_t kpe = ALIGN_UP(kps + ((uint64_t)__kernel_end - (uint64_t)__kernel_start), PAGE_SIZE);
	uint64_t cs = 0, ce = 0;
	for (size_t i = 0; i <= bootinfo.nmmap; i++) {
		const struct boot_mmap_entry *m = i < bootinfo.nmmap ? &bootinfo.mmap[i] : NULL;
		if (m && !dmap_wb_type(m->type))
			continue;
		uint64_t s = m ? ALIGN_DOWN(m->base, PAGE_SIZE) : ~0ULL;
		uint64_t e = m ? ALIGN_UP(m->base + m->length, PAGE_SIZE) : ~0ULL;
		if (m && ce > cs && s <= ce) {
			ce = MAX(ce, e);
			continue;
		}
		if (ce > cs) {
			dmap_wb(cs, ce, kps, kpe);
			vmm_dmap_bytes += ce - cs;
		}
		cs = s;
		ce = e;
	}
	/* the framebuffer, write-combining (PAT entry 5, as Limine programs the PAT) */
	if (bootinfo.fb.pa) {
		uint64_t len = bootinfo.fb.pitch * bootinfo.fb.height;
		dmap_range(bootinfo.fb.pa, bootinfo.fb.pa + len, PTE_W | PTE_G | nx() | PTE_PAT | PTE_PWT, false);
	}
	write_cr3(virt_to_phys(kernel_pml4));
	kprintf("vmm: W^X kernel page tables active (text RX, rodata R, data RW, NX %s), direct map %lu MiB, %lu table pages\n",
		nx_enabled ? "on" : "off", vmm_dmap_bytes >> 20, vmm_pt_pages);
}

void *vmm_map_mmio(uint64_t pa, uint64_t len)
{
	uint64_t s = ALIGN_DOWN(pa, PAGE_SIZE), e = ALIGN_UP(pa + len, PAGE_SIZE);
	for (uint64_t p = s; p < e; p += PAGE_SIZE) {
		uint64_t f;
		if (vmm_translate(dmap_base + p, NULL, &f))
			continue; /* already covered (e.g. a firmware region) */
		if (vmm_map(dmap_base + p, p, PTE_W | PTE_G | PTE_PCD | PTE_PWT | PTE_NX) != 0)
			return NULL;
	}
	return phys_to_virt(pa);
}

/* Make the direct-map alias of page-table page `pa` read-only, splitting
 * a 2 MiB direct-map page into 4 KiB pages first if needed. */
static void dmap_make_ro(uint64_t pa)
{
	uint64_t va = dmap_base + pa;
	uint64_t *pde = walk(kernel_pml4, va, 2, false);
	if (!pde || !(*pde & PTE_P))
		panic("vmm: page table %p is not in the direct map", (void *)pa);
	if (*pde & PTE_PS) {
		uint64_t base = *pde & PTE_ADDR_MASK & ~((2ULL << 20) - 1);
		uint64_t fl = *pde & ~PTE_ADDR_MASK & ~PTE_PS; /* bit 12 (PAT) is clear for WB */
		uint64_t *pt = pt_alloc();
		for (int i = 0; i < 512; i++)
			pt[i] = (base + (uint64_t)i * PAGE_SIZE) | fl;
		*pde = virt_to_phys(pt) | PTE_P | PTE_W;
		write_cr3(read_cr3()); /* drop the 2 MiB TLB entry (and global ones below) */
	}
	uint64_t *pte = walk(kernel_pml4, va, 1, false);
	*pte &= ~PTE_W;
	invlpg(va);
}

/* Lock the kernel window: see "Immutable kernel image" above. */
void vmm_lock_kernel(void)
{
	immutable_start = 0xffffff8000000000ULL; /* PML4 slot 511 */
	KASSERT(kernel_virt_base >= immutable_start);
	uint64_t pml4e = kernel_pml4[511];
	uint64_t ro_pages = 0;
	/* every table below PML4[511]: the PDPT, the PDs, the PTs (4 KiB only) */
	uint64_t *pdpt = phys_to_virt(pml4e & PTE_ADDR_MASK);
	spin_lock(&vmm_lock);
	for (int i = 0; i < 512; i++) {
		if (!(pdpt[i] & PTE_P) || (pdpt[i] & PTE_PS))
			continue;
		uint64_t *pd = phys_to_virt(pdpt[i] & PTE_ADDR_MASK);
		for (int j = 0; j < 512; j++)
			if ((pd[j] & PTE_P) && !(pd[j] & PTE_PS)) {
				dmap_make_ro(pd[j] & PTE_ADDR_MASK);
				ro_pages++;
			}
		dmap_make_ro(pdpt[i] & PTE_ADDR_MASK);
		ro_pages++;
	}
	dmap_make_ro(pml4e & PTE_ADDR_MASK);
	ro_pages++;
	kernel_locked = true;
	spin_unlock(&vmm_lock);
	kprintf("vmm: kernel image locked: %lu page-table pages read-only, remaps refused\n", ro_pages);
}

bool vmm_kernel_locked(void) { return kernel_locked; }

static void vmm_lock_sysinit(void) { vmm_lock_kernel(); }
SYSINIT(vmm_lock, SI_ORDER_LAST, vmm_lock_sysinit); /* last boot step */

static void vmm_sysinit(void) { vmm_init(); }
SYSINIT(vmm, 20001, vmm_sysinit); /* right after pmm (SI_ORDER_VM) */
