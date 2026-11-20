/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * vmm.c - 4-level page tables with W^X.
 *
 * Replaces the boot tables (which map 1 GiB RWX three times) with:
 *   .text    RX          (4 KiB pages, global)
 *   .rodata  R  + NX
 *   .data/.bss RW + NX
 *   direct map of RAM at PHYS_MAP_BASE: RW + NX (2 MiB pages), except the
 *     frames of the kernel image itself, which are read-only there, so the
 *     image has no writable alias of its code.
 *   nothing at virtual address 0 (NULL dereferences fault).
 * No mapping is ever both writable and executable (checked by ktest 1xx).
 * User windows (uvm.c) live below USER_TOP with the U bit.
 */
#include <magia/cpu.h>
#include <magia/linker_set.h>
#include <magia/mm.h>
#include <magia/spinlock.h>
#include <magia/x86.h>

extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[];
extern char __data_start[], __bss_end[], __kernel_phys_start[], __kernel_phys_end[];

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

int vmm_map(uint64_t va, uint64_t pa, uint64_t flags)
{
	if (!nx_enabled)
		flags &= ~PTE_NX;
	if ((flags & PTE_W) && !(flags & PTE_NX) && nx_enabled)
		panic("vmm_map: refusing W+X mapping at %p", (void *)va);
	spin_lock(&vmm_lock);
	uint64_t *pte = walk(kernel_pml4, va, 1, true);
	if (!pte) {
		spin_unlock(&vmm_lock);
		return -1;
	}
	*pte = (pa & PTE_ADDR_MASK) | flags | PTE_P;
	invlpg(va);
	spin_unlock(&vmm_lock);
	return 0;
}

uint64_t vmm_unmap(uint64_t va)
{
	spin_lock(&vmm_lock);
	uint64_t *pte = walk(kernel_pml4, va, 1, false);
	uint64_t old = 0;
	if (pte && (*pte & PTE_P)) {
		old = *pte & PTE_ADDR_MASK;
		*pte = 0;
		invlpg(va);
	}
	spin_unlock(&vmm_lock);
	return old;
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

	/* direct map */
	uint64_t kps = ALIGN_DOWN((uint64_t)__kernel_phys_start, PAGE_SIZE);
	uint64_t kpe = ALIGN_UP((uint64_t)__kernel_phys_end, PAGE_SIZE);
	uint64_t top = ALIGN_UP(pmm_max_phys(), 2ULL << 20);
	for (uint64_t pa = 0; pa < top; pa += 2ULL << 20) {
		uint64_t va = PHYS_MAP_BASE + pa;
		if (pa + (2ULL << 20) <= kps || pa >= kpe) {
			uint64_t *pde = walk(kernel_pml4, va, 2, true);
			*pde = pa | PTE_P | PTE_W | PTE_PS | PTE_G | nx();
		} else {
			for (uint64_t p = pa; p < pa + (2ULL << 20); p += PAGE_SIZE) {
				uint64_t *pte = walk(kernel_pml4, PHYS_MAP_BASE + p, 1, true);
				bool image = p >= kps && p < kpe;
				*pte = p | PTE_P | PTE_G | nx() | (image ? 0 : PTE_W);
			}
		}
	}
	write_cr3(virt_to_phys(kernel_pml4));
	kprintf("vmm: W^X kernel page tables active (text RX, rodata R, data RW, NX %s), %lu table pages\n",
		nx_enabled ? "on" : "off", vmm_pt_pages);
}

static void vmm_sysinit(void) { vmm_init(); }
SYSINIT(vmm, 20001, vmm_sysinit); /* right after pmm (SI_ORDER_VM) */
