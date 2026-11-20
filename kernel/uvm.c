/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * uvm.c - user windows and user-memory access.
 *
 * Magia 0.1 has a single address space. "User windows" are page ranges
 * below USER_TOP mapped with the U bit (RW+NX for data, R+X for code); every
 * page is charged to a jail through rs_jail_mem_charge() so jail memory
 * limits are enforced where memory is actually handed out.
 *
 * copyin/copyout are the only way kernel code touches user memory:
 *   1. range check: no overflow, entirely below USER_TOP;
 *   2. every page present with U (and W for copyout) in the page tables;
 *   3. open the SMAP window (stac), copy with uaccess_copy (a page fault
 *      inside it returns -EFAULT instead of panicking), close it (clac).
 * Any direct kernel access to a U page outside that window faults (SMAP),
 * and the kernel can never execute U pages (SMEP).
 */
#include <magia/abi.h>
#include <magia/cpu.h>
#include <magia/mm.h>
#include <magia/rust.h>
#include <magia/spinlock.h>
#include <magia/sysctl.h>
#include <magia/x86.h>

long uaccess_copy(void *dst, const void *src, size_t n);

static spinlock_t uvm_lock = SPINLOCK_INIT("uvm");
static uint64_t uvm_next = USER_WIN_BASE;
static uint64_t uvm_pages_mapped;
SYSCTL_U64_RO(vm_uvm_pages, "vm.uvm.pages_mapped", &uvm_pages_mapped, "pages mapped in user windows");

void uaccess_begin(void)
{
	if (smap_enabled)
		__asm__ volatile("stac" ::: "memory", "cc");
}

void uaccess_end(void)
{
	if (smap_enabled)
		__asm__ volatile("clac" ::: "memory", "cc");
}

static int free_pages_range(uint64_t uva, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		uint64_t pa = 0;
		if (vmm_unmap(uva + i * PAGE_SIZE, &pa) == 0) {
			pmm_free(pa, 0);
			uvm_pages_mapped--;
		}
	}
	return 0;
}

int uvm_alloc(int jid, uint64_t npages, int flags, uint64_t *uva_out, uint64_t *kva_out)
{
	if (npages == 0 || npages > (MMAP_MAX_BYTES / PAGE_SIZE) * 4)
		return -EINVAL;
	if (rs_jail_mem_charge(jid, npages * PAGE_SIZE) != 0)
		return -ENOMEM;

	spin_lock(&uvm_lock);
	uint64_t uva = uvm_next;
	uvm_next += (npages + 1) * PAGE_SIZE; /* one unmapped guard page after each window */
	spin_unlock(&uvm_lock);

	uint64_t pte = PTE_U | ((flags & UVM_EXEC) ? 0 : (PTE_W | PTE_NX));
	uint64_t kva = 0;
	if (flags & UVM_CONTIG) {
		int order = 0;
		while ((1ULL << order) < npages)
			order++;
		uint64_t pa = order <= MAX_ORDER ? pmm_alloc(order) : 0;
		if (!pa)
			goto nomem;
		/* give back the unused tail of the buddy block */
		for (uint64_t i = npages; i < (1ULL << order); i++)
			pmm_free(pa + i * PAGE_SIZE, 0);
		memset(phys_to_virt(pa), 0, npages * PAGE_SIZE);
		for (uint64_t i = 0; i < npages; i++) {
			pa_to_page(pa + i * PAGE_SIZE)->flags |= PG_USER;
			if (vmm_map(uva + i * PAGE_SIZE, pa + i * PAGE_SIZE, pte) != 0)
				panic("uvm: map failed");
			uvm_pages_mapped++;
		}
		kva = (uint64_t)phys_to_virt(pa);
	} else {
		for (uint64_t i = 0; i < npages; i++) {
			uint64_t pa = pmm_alloc(0);
			if (!pa) {
				free_pages_range(uva, i);
				goto nomem;
			}
			memset(phys_to_virt(pa), 0, PAGE_SIZE);
			pa_to_page(pa)->flags |= PG_USER;
			if (vmm_map(uva + i * PAGE_SIZE, pa, pte) != 0)
				panic("uvm: map failed");
			uvm_pages_mapped++;
		}
	}
	*uva_out = uva;
	if (kva_out)
		*kva_out = kva;
	return 0;
nomem:
	rs_jail_mem_uncharge(jid, npages * PAGE_SIZE);
	return -ENOMEM;
}

int uvm_free(int jid, uint64_t uva, uint64_t npages)
{
	if (uva < USER_WIN_BASE || uva >= USER_TOP || (uva & (PAGE_SIZE - 1)) || npages == 0)
		return -EINVAL;
	for (uint64_t i = 0; i < npages; i++) {
		uint64_t fl;
		if (!vmm_translate(uva + i * PAGE_SIZE, NULL, &fl) || !(fl & PTE_U))
			return -EINVAL;
	}
	free_pages_range(uva, npages);
	rs_jail_mem_uncharge(jid, npages * PAGE_SIZE);
	return 0;
}

static bool range_ok(uint64_t uva, uint64_t len, bool write)
{
	if (len == 0)
		return true;
	if (uva + len < uva || uva + len > USER_TOP)
		return false;
	for (uint64_t p = ALIGN_DOWN(uva, PAGE_SIZE); p < uva + len; p += PAGE_SIZE) {
		uint64_t fl;
		if (!vmm_translate(p, NULL, &fl) || !(fl & PTE_U))
			return false;
		if (write && !(fl & PTE_W))
			return false;
	}
	return true;
}

bool uvm_range_mapped(uint64_t uva, uint64_t len) { return range_ok(uva, len, false); }

int copyin(void *kdst, uint64_t usrc, size_t n)
{
	if (!range_ok(usrc, n, false))
		return -EFAULT;
	uaccess_begin();
	long r = uaccess_copy(kdst, (const void *)usrc, n);
	uaccess_end();
	return (int)r;
}

int copyout(uint64_t udst, const void *ksrc, size_t n)
{
	if (!range_ok(udst, n, true))
		return -EFAULT;
	uaccess_begin();
	long r = uaccess_copy((void *)udst, ksrc, n);
	uaccess_end();
	return (int)r;
}
