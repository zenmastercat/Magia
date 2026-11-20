/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * t_mm.c - ktests for the page allocator, kmalloc, W^X/NX/SMEP/SMAP and
 * user-memory access (1xx). The protection tests use the expected-fault
 * mechanism (ksetjmp + curthread()->fault_jmp): the access must fault, and
 * the kernel must survive it.
 */
#include <magia/abi.h>
#include <magia/boot.h>
#include <magia/cpu.h>
#include <magia/ktest.h>
#include <magia/mm.h>
#include <magia/thread.h>
#include <magia/trap.h>
#include <magia/x86.h>

long uaccess_copy(void *dst, const void *src, size_t n);

#define PF (14 + 1) /* ksetjmp value for a page fault */

static __attribute__((noinline)) void do_write8(uint64_t a)
{
	volatile uint8_t *p = (volatile uint8_t *)a;
	*p = *p;
}
static __attribute__((noinline)) void do_read8(uint64_t a) { (void)*(volatile uint8_t *)a; }
static __attribute__((noinline)) void do_exec(uint64_t a) { ((void (*)(void))a)(); }

/* Run fn(arg); return 0 if it completed, or vector+1 if it faulted. */
static int try_access(void (*fn)(uint64_t), uint64_t arg)
{
	struct kjmp j;
	struct thread *volatile t = curthread();
	int v = ksetjmp(&j);
	if (v == 0) {
		t->fault_jmp = &j;
		fn(arg);
		t->fault_jmp = NULL;
		return 0;
	}
	t->fault_jmp = NULL;
	return v;
}

KTEST(100, pmm_alloc_free_restores_count)
{
	uint64_t before = pmm_free_pages();
	uint64_t pa[MAX_ORDER + 1];
	for (int o = 0; o <= 6; o++) {
		pa[o] = pmm_alloc(o);
		KT_ASSERT(pa[o] != 0);
		KT_EQ(pa[o] & ((PAGE_SIZE << o) - 1), 0); /* naturally aligned */
	}
	KT_EQ(pmm_free_pages(), before - 127);
	for (int o = 0; o <= 6; o++)
		pmm_free(pa[o], o);
	KT_EQ(pmm_free_pages(), before);
	KT_EQ(pmm_alloc(MAX_ORDER + 1), 0); /* negative: order too large */
	KT_EQ(pmm_alloc(-1), 0);
}

KTEST(101, pmm_buddy_merge)
{
	/* Free an order-4 block as 16 single pages: the buddies must merge
	 * back into one free block of order >= 4 covering it. */
	uint64_t big = pmm_alloc(4);
	KT_ASSERT(big != 0);
	uint64_t before = pmm_free_pages();
	for (int i = 0; i < 16; i++)
		pmm_free(big + i * PAGE_SIZE, 0);
	KT_EQ(pmm_free_pages(), before + 16);
	bool merged = false;
	for (int o = 4; o <= MAX_ORDER && !merged; o++) {
		struct page *head = pa_to_page(ALIGN_DOWN(big, PAGE_SIZE << o));
		merged = head && (head->flags & PG_FREE) && head->order >= o;
	}
	KT_ASSERT(merged);
	KT_ASSERT(!(pa_to_page(big + PAGE_SIZE)->flags & PG_FREE)); /* not a separate head */
}

static uint64_t rng = 0x9e3779b97f4a7c15ULL;
static uint64_t xorshift(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

KTEST(102, kmem_stress_no_leak)
{
	enum { SLOTS = 64 };
	static uint8_t *ptr[SLOTS];
	static size_t len[SLOTS];
	kmem_reclaim();
	uint64_t before = pmm_free_pages();
	struct kmem_stats s0;
	kmem_get_stats(&s0);
	int corrupt = 0;
	for (int op = 0; op < 4000; op++) {
		int i = (int)(xorshift() % SLOTS);
		if (ptr[i]) {
			for (size_t k = 0; k < len[i]; k++)
				if (ptr[i][k] != (uint8_t)(i + k))
					corrupt++;
			kfree(ptr[i]);
			ptr[i] = NULL;
		} else {
			len[i] = 1 + xorshift() % ((xorshift() & 7) ? 300 : 9000);
			ptr[i] = kmalloc(len[i]);
			KT_ASSERT(ptr[i] != NULL);
			for (size_t k = 0; k < len[i]; k++)
				ptr[i][k] = (uint8_t)(i + k);
		}
	}
	for (int i = 0; i < SLOTS; i++) {
		kfree(ptr[i]);
		ptr[i] = NULL;
	}
	KT_EQ(corrupt, 0);
	kmem_reclaim();
	struct kmem_stats s1;
	kmem_get_stats(&s1);
	KT_EQ(s1.bytes_inuse, s0.bytes_inuse);
	KT_EQ(pmm_free_pages(), before);
}

KTEST(103, kmem_alignment)
{
	size_t aligns[] = { 16, 64, 256, 1024, 4096, 16384 };
	for (size_t i = 0; i < ARRAY_SIZE(aligns); i++) {
		void *p = kmalloc_aligned(24, aligns[i]);
		KT_ASSERT(p != NULL);
		KT_EQ((uint64_t)p & (aligns[i] - 1), 0);
		kfree(p);
	}
	void *z = kzalloc(100);
	KT_ASSERT(z && ((uint8_t *)z)[99] == 0);
	kfree(z);
	kfree(NULL); /* no-op */
}

KTEST(110, wx_text_is_not_writable)
{
	uint64_t text = (uint64_t)ktest_run_all;
	KT_EQ(try_access(do_write8, text), PF);
	KT_EQ(curthread()->fault_addr, text);
	KT_EQ(try_access(do_read8, text), 0); /* positive: readable */
}

KTEST(111, rodata_is_not_writable)
{
	static const char ro[] = "read only";
	KT_EQ(try_access(do_write8, (uint64_t)ro), PF);
	KT_EQ(try_access(do_read8, (uint64_t)ro), 0);
}

KTEST(112, data_and_stack_are_not_executable)
{
	static uint8_t data_ret[16] = { 0xc3 }; /* `ret` */
	volatile uint8_t stack_ret[16] = { 0xc3 };
	if (!nx_enabled)
		return;
	KT_EQ(try_access(do_exec, (uint64_t)data_ret), PF);
	KT_EQ(try_access(do_exec, (uint64_t)stack_ret), PF);
	uint8_t *heap = kmalloc(16);
	heap[0] = 0xc3;
	KT_EQ(try_access(do_exec, (uint64_t)heap), PF);
	kfree(heap);
}

KTEST(113, null_and_unmapped_addresses_fault)
{
	KT_EQ(try_access(do_read8, 0), PF);
	KT_EQ(try_access(do_read8, 0xfff), PF);
	KT_EQ(try_access(do_write8, 0x200000), PF);
	extern char __kernel_end[];
	KT_EQ(try_access(do_read8, ALIGN_UP((uint64_t)__kernel_end, 2ULL << 20) + PAGE_SIZE), PF); /* beyond the image */
}

KTEST(114, direct_map_alias_of_kernel_is_read_only)
{
	uint64_t alias = (uint64_t)phys_to_virt(virt_to_phys((void *)ktest_run_all));
	KT_EQ(try_access(do_read8, alias), 0);
	KT_EQ(try_access(do_write8, alias), PF);
	if (nx_enabled)
		KT_EQ(try_access(do_exec, alias), PF);
}

/* Walk every present leaf of the live page tables: none may be W and X. */
KTEST(115, page_table_scan_finds_no_wx_mapping)
{
	uint64_t *pml4 = phys_to_virt(read_cr3() & PTE_ADDR_MASK);
	uint64_t leaves = 0, wx = 0, text_x = 0;
	for (int i = 0; i < 512; i++) {
		if (!(pml4[i] & PTE_P))
			continue;
		uint64_t *pdpt = phys_to_virt(pml4[i] & PTE_ADDR_MASK);
		for (int j = 0; j < 512; j++) {
			if (!(pdpt[j] & PTE_P))
				continue;
			if (pdpt[j] & PTE_PS) {
				leaves++;
				wx += (pdpt[j] & PTE_W) && !(pdpt[j] & PTE_NX);
				continue;
			}
			uint64_t *pd = phys_to_virt(pdpt[j] & PTE_ADDR_MASK);
			for (int k = 0; k < 512; k++) {
				if (!(pd[k] & PTE_P))
					continue;
				if (pd[k] & PTE_PS) {
					leaves++;
					wx += (pd[k] & PTE_W) && !(pd[k] & PTE_NX);
					continue;
				}
				uint64_t *pt = phys_to_virt(pd[k] & PTE_ADDR_MASK);
				for (int l = 0; l < 512; l++) {
					uint64_t e = pt[l];
					if (!(e & PTE_P))
						continue;
					leaves++;
					bool x = !(e & PTE_NX);
					wx += (e & PTE_W) && x;
					text_x += x && !(e & PTE_U);
				}
			}
		}
	}
	KT_ASSERT(leaves > 100);
	KT_EQ(wx, 0);
	KT_ASSERT(text_x > 0); /* the scan really saw executable kernel text */
	if (nx_enabled) {
		extern char __text_start[], __text_end[];
		KT_EQ(text_x, ((uint64_t)__text_end - (uint64_t)__text_start) / PAGE_SIZE);
	}
}

KTEST(116, smap_blocks_kernel_access_to_user_pages)
{
	uint64_t uva = 0;
	KT_EQ(uvm_alloc(0, 1, 0, &uva, NULL), 0);
	int expect = smap_enabled ? PF : 0;
	KT_EQ(try_access(do_read8, uva), expect);
	KT_EQ(try_access(do_write8, uva), expect);
	uint32_t v = 0xfeedf00d, r = 0;
	KT_EQ(copyout(uva, &v, 4), 0); /* the sanctioned path works */
	KT_EQ(copyin(&r, uva, 4), 0);
	KT_EQ(r, 0xfeedf00d);
	KT_EQ(uvm_free(0, uva, 1), 0);
}

KTEST(117, smep_blocks_kernel_execution_of_user_pages)
{
	uint64_t uva = 0, kva = 0;
	KT_EQ(uvm_alloc(0, 1, UVM_EXEC | UVM_CONTIG, &uva, &kva), 0);
	KT_ASSERT(kva != 0);
	*(uint8_t *)kva = 0xc3;
	if (smep_enabled)
		KT_EQ(try_access(do_exec, uva), PF);
	uint64_t fl = 0;
	KT_ASSERT(vmm_translate(uva, NULL, &fl));
	KT_ASSERT((fl & PTE_U) && !(fl & PTE_W) && !(fl & PTE_NX)); /* user R+X only */
	KT_EQ(uvm_free(0, uva, 1), 0);
}

KTEST(118, copyin_copyout_reject_bad_pointers)
{
	uint64_t uva = 0, x = 0;
	KT_EQ(uvm_alloc(0, 2, 0, &uva, NULL), 0);
	KT_EQ(copyin(&x, (uint64_t)&x, 8), -EFAULT);              /* kernel address */
	KT_EQ(copyin(&x, 0x1000, 8), -EFAULT);                    /* unmapped user page */
	KT_EQ(copyin(&x, 0xfffffffffffff000ULL, 0x2000), -EFAULT); /* wraps around */
	KT_EQ(copyin(&x, USER_TOP - 4, 8), -EFAULT);              /* crosses USER_TOP */
	KT_EQ(copyout(uva + 2 * PAGE_SIZE - 4, &x, 8), -EFAULT);  /* into the guard page */
	KT_EQ(copyout(uva + PAGE_SIZE - 4, &x, 8), 0);            /* across two mapped pages */
	KT_EQ(copyin(&x, uva, 0), 0);                             /* empty copy */
	uint64_t code = 0, kva = 0;
	KT_EQ(uvm_alloc(0, 1, UVM_EXEC | UVM_CONTIG, &code, &kva), 0);
	KT_EQ(copyout(code, &x, 8), -EFAULT);                     /* user code is read-only */
	KT_EQ(copyin(&x, code, 8), 0);
	KT_EQ(uvm_free(0, code, 1), 0);
	KT_EQ(uvm_free(0, uva, 2), 0);
	KT_EQ(copyin(&x, uva, 8), -EFAULT);                       /* after unmap */
}

KTEST(119, uaccess_fault_fixup_returns_efault)
{
	uint64_t buf = 0;
	uaccess_begin();
	long r = uaccess_copy(&buf, (const void *)0x2000, 8); /* unchecked: must not panic */
	uaccess_end();
	KT_EQ(r, -EFAULT);
}

KTEST(120, uvm_free_rejects_foreign_ranges)
{
	uint64_t uva = 0;
	KT_EQ(uvm_free(0, (uint64_t)&uva, 1), -EINVAL); /* kernel address */
	KT_EQ(uvm_free(0, USER_WIN_BASE + 123, 1), -EINVAL); /* unaligned */
	KT_EQ(uvm_alloc(0, 1, 0, &uva, NULL), 0);
	KT_EQ(uvm_free(0, uva, 1), 0);
	KT_EQ(uvm_free(0, uva, 1), -EINVAL); /* double free */
	KT_EQ(uvm_alloc(0, 0, 0, &uva, NULL), -EINVAL);
}

/* All RAM is managed (no 1 GiB cap): allocate and touch frames above 4 GiB.
 * The test boots with -m 8G, so RAM above 4 GiB exists. */
KTEST(121, pmm_pages_above_4g)
{
	KT_ASSERT(pmm_max_phys() > (4ULL << 30));
	enum { N = 1024 };
	static uint64_t blk[N];
	int n = 0, found = -1;
	while (n < N && found < 0) {
		uint64_t pa = pmm_alloc(MAX_ORDER);
		if (!pa)
			break;
		blk[n] = pa;
		if (pa >= (4ULL << 30))
			found = n;
		n++;
	}
	KT_ASSERT(found >= 0);
	if (found >= 0) {
		uint64_t pa = blk[found];
		volatile uint64_t *p = phys_to_virt(pa);
		volatile uint64_t *q = phys_to_virt(pa + (PAGE_SIZE << MAX_ORDER) - 8);
		*p = pa ^ 0x5a5a5a5a5a5a5a5aULL;
		*q = ~pa;
		KT_EQ(*p, pa ^ 0x5a5a5a5a5a5a5a5aULL);
		KT_EQ(*q, ~pa);
		uint64_t tpa = 0;
		KT_ASSERT(vmm_translate((uint64_t)p, &tpa, NULL));
		KT_EQ(tpa, pa);
	}
	for (int i = 0; i < n; i++)
		pmm_free(blk[i], MAX_ORDER);
}

/* Bootloader-reclaimable memory joined the allocator after the switch to
 * Magia's page tables, and the allocator manages (nearly) all usable RAM. */
KTEST(122, pmm_reclaims_bootloader_memory)
{
	extern uint64_t pmm_reclaimed_pages;
	KT_ASSERT(pmm_reclaimed_pages > 0);
	uint64_t ram = 0;
	for (size_t i = 0; i < bootinfo.nmmap; i++)
		if (bootinfo.mmap[i].type == MEM_USABLE || bootinfo.mmap[i].type == MEM_BOOT_RECLAIM)
			ram += bootinfo.mmap[i].length;
	uint64_t managed = pmm_total_pages() * PAGE_SIZE;
	uint64_t array = ALIGN_UP((pmm_max_phys() >> PAGE_SHIFT) * sizeof(struct page), PAGE_SIZE);
	KT_ASSERT(managed <= ram);
	KT_ASSERT(managed + array + (1ULL << 20) >= ram); /* minus page array and low 1 MiB */
	/* the direct map reaches the top of RAM */
	KT_ASSERT(vmm_translate((uint64_t)phys_to_virt(pmm_max_phys() - PAGE_SIZE), NULL, NULL));
}

/* Immutable kernel image: after boot the kernel window's mappings cannot
 * be changed through the VM interface... */
KTEST(130, kernel_image_remap_is_refused)
{
	extern char __text_start[], __rodata_start[], __data_start[];
	KT_ASSERT(vmm_kernel_locked());
	uint64_t text = (uint64_t)__text_start, ro = (uint64_t)__rodata_start, data = (uint64_t)__data_start;
	uint64_t pa0 = 0, fl0 = 0, pa1 = 0, fl1 = 0;
	KT_ASSERT(vmm_translate(text, &pa0, &fl0));
	uint64_t spare = pmm_alloc(0);
	KT_EQ(vmm_map(text, spare, PTE_G), -EPERM);              /* replace code */
	KT_EQ(vmm_map(ro, spare, PTE_G | PTE_NX), -EPERM);
	KT_EQ(vmm_protect(ro, PTE_W | PTE_NX | PTE_G), -EPERM);  /* make rodata writable */
	KT_EQ(vmm_protect(text, PTE_G | PTE_NX), -EPERM);
	KT_EQ(vmm_unmap(data, NULL), -EPERM);
	KT_EQ(vmm_map(KERNEL_VMA - PAGE_SIZE, spare, PTE_W | PTE_NX), -EPERM); /* new page in the window */
	KT_ASSERT(vmm_translate(text, &pa1, &fl1));
	KT_EQ(pa1, pa0);
	KT_EQ(fl1, fl0);
	/* the rest of the address space still works: a user window maps and unmaps */
	uint64_t uva = 0;
	KT_EQ(uvm_alloc(0, 1, 0, &uva, NULL), 0);
	KT_EQ(vmm_protect(uva, PTE_U | PTE_NX), 0);
	KT_EQ(vmm_protect(uva, PTE_U | PTE_W | PTE_NX), 0);
	KT_EQ(uvm_free(0, uva, 1), 0);
	KT_EQ(vmm_unmap(uva, NULL), -ENOENT);
	KT_EQ(vmm_protect(uva, PTE_U | PTE_NX), -ENOENT);
	pmm_free(spare, 0);
}

/* ...and its page-table pages are read-only: a direct write to the PTE of
 * a text page, its PD entry or PDPT entry faults. */
KTEST(131, kernel_page_tables_are_read_only)
{
	extern char __text_start[], __data_start[];
	uint64_t pml4e = ((uint64_t *)phys_to_virt(read_cr3() & PTE_ADDR_MASK))[511];
	uint64_t text = (uint64_t)__text_start;
	uint64_t *pdpt = phys_to_virt(pml4e & PTE_ADDR_MASK);
	uint64_t *pdpte = &pdpt[(text >> 30) & 511];
	uint64_t *pd = phys_to_virt(*pdpte & PTE_ADDR_MASK);
	uint64_t *pde = &pd[(text >> 21) & 511];
	uint64_t *pt = phys_to_virt(*pde & PTE_ADDR_MASK);
	uint64_t *pte = &pt[(text >> 12) & 511];
	KT_EQ(*pte & PTE_ADDR_MASK, virt_to_phys(__text_start)); /* found the right entry */
	KT_EQ(try_access(do_read8, (uint64_t)pte), 0);
	KT_EQ(try_access(do_write8, (uint64_t)pte), PF);
	KT_EQ(try_access(do_write8, (uint64_t)pde), PF);
	KT_EQ(try_access(do_write8, (uint64_t)pdpte), PF);
	/* the PTE of a data page (writable data, read-only table) as well */
	uint64_t d = (uint64_t)__data_start;
	uint64_t *dpt = phys_to_virt(pd[(d >> 21) & 511] & PTE_ADDR_MASK);
	KT_EQ(try_access(do_write8, (uint64_t)&dpt[(d >> 12) & 511]), PF);
	/* nothing changed */
	uint64_t pa = 0;
	KT_ASSERT(vmm_translate(text, &pa, NULL));
	KT_EQ(pa, virt_to_phys(__text_start));
}
