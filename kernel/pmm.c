/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * pmm.c - physical memory: a binary buddy allocator over the Limine
 * memory map (all of RAM, no size cap), with one struct page per 4 KiB frame.
 *
 * Idea: the classic buddy system (Knowlton 1965), as used by Linux's page
 * allocator; FreeBSD's vm_phys uses the same idea. A free block of order k
 * is 2^k pages, aligned to its size; its buddy is at pfn ^ (1 << k).
 * Freeing merges with the buddy while the buddy is a free block of the same
 * order. Free lists are doubly linked through struct page.
 */
#include <magia/boot.h>
#include <magia/linker_set.h>
#include <magia/mm.h>
#include <magia/spinlock.h>
#include <magia/sysctl.h>

static struct page *pages;
static uint64_t npages;          /* frames covered by `pages` */
static uint64_t total_pages;     /* usable frames handed to the allocator */
static volatile uint64_t free_pages;
static struct page *free_list[MAX_ORDER + 1];
static uint64_t free_count[MAX_ORDER + 1];
static spinlock_t pmm_lock = SPINLOCK_INIT("pmm");
static uint64_t max_phys;

SYSCTL_U64_RO(vm_free_pages_c, "vm.pmm.free_pages", &free_pages, "free 4 KiB frames (buddy)");
SYSCTL_U64_RO(vm_total_pages_c, "vm.pmm.total_pages", &total_pages, "usable 4 KiB frames");

struct page *pa_to_page(uint64_t pa)
{
	uint64_t pfn = pa >> PAGE_SHIFT;
	return pfn < npages ? &pages[pfn] : NULL;
}

uint64_t page_to_pa(const struct page *pg) { return (uint64_t)(pg - pages) << PAGE_SHIFT; }

static void list_push(int order, struct page *pg)
{
	pg->prev = NULL;
	pg->next = free_list[order];
	if (pg->next)
		pg->next->prev = pg;
	free_list[order] = pg;
	pg->flags |= PG_FREE;
	pg->order = (int8_t)order;
	free_count[order]++;
}

static void list_remove(int order, struct page *pg)
{
	if (pg->prev)
		pg->prev->next = pg->next;
	else
		free_list[order] = pg->next;
	if (pg->next)
		pg->next->prev = pg->prev;
	pg->next = pg->prev = NULL;
	pg->flags &= ~PG_FREE;
	free_count[order]--;
}

static void free_locked(uint64_t pfn, int order)
{
	free_pages += 1ULL << order;
	while (order < MAX_ORDER) {
		uint64_t buddy = pfn ^ (1ULL << order);
		if (buddy + (1ULL << order) > npages)
			break;
		struct page *b = &pages[buddy];
		if (!(b->flags & PG_FREE) || b->order != order)
			break;
		list_remove(order, b);
		pfn &= ~(1ULL << order);
		order++;
	}
	list_push(order, &pages[pfn]);
}

uint64_t pmm_alloc(int order)
{
	if (order < 0 || order > MAX_ORDER)
		return 0;
	spin_lock(&pmm_lock);
	int o = order;
	while (o <= MAX_ORDER && !free_list[o])
		o++;
	if (o > MAX_ORDER) {
		spin_unlock(&pmm_lock);
		return 0;
	}
	struct page *pg = free_list[o];
	list_remove(o, pg);
	uint64_t pfn = (uint64_t)(pg - pages);
	while (o > order) { /* split, keep the low half */
		o--;
		list_push(o, &pages[pfn + (1ULL << o)]);
	}
	pg->order = (int8_t)order;
	pg->flags &= ~(PG_SLAB | PG_LARGE | PG_USER);
	free_pages -= 1ULL << order;
	spin_unlock(&pmm_lock);
	return pfn << PAGE_SHIFT;
}

void pmm_free(uint64_t pa, int order)
{
	KASSERT((pa & (PAGE_SIZE - 1)) == 0);
	uint64_t pfn = pa >> PAGE_SHIFT;
	KASSERT(pfn < npages && order >= 0 && order <= MAX_ORDER);
	struct page *pg = &pages[pfn];
	if (pg->flags & (PG_FREE | PG_RESERVED))
		panic("pmm_free: bad free of pa %p (flags %x)", (void *)pa, pg->flags);
	spin_lock(&pmm_lock);
	pg->flags &= ~(PG_SLAB | PG_LARGE | PG_USER);
	free_locked(pfn, order);
	spin_unlock(&pmm_lock);
}

void *page_alloc(int order)
{
	uint64_t pa = pmm_alloc(order);
	if (!pa)
		return NULL;
	void *va = phys_to_virt(pa);
	memset(va, 0, PAGE_SIZE << order);
	return va;
}

void page_free(void *va, int order) { pmm_free(virt_to_phys(va), order); }

uint64_t pmm_free_pages(void) { return free_pages; }
uint64_t pmm_total_pages(void) { return total_pages; }
uint64_t pmm_max_phys(void) { return max_phys; }

void pmm_dump(void)
{
	kprintf("pmm: %lu/%lu pages free (%lu KiB), max phys %p\n", free_pages, total_pages,
		free_pages * 4, (void *)max_phys);
	kprintf("pmm: free blocks per order:");
	for (int o = 0; o <= MAX_ORDER; o++)
		kprintf(" %lu", free_count[o]);
	kprintf("\n");
}

/* Add frames [start, end) to the allocator as maximal aligned blocks. */
static void add_range(uint64_t start, uint64_t end)
{
	uint64_t pfn = ALIGN_UP(start, PAGE_SIZE) >> PAGE_SHIFT;
	uint64_t last = ALIGN_DOWN(end, PAGE_SIZE) >> PAGE_SHIFT;
	last = MIN(last, npages);
	while (pfn < last) {
		int order = 0;
		while (order < MAX_ORDER && !(pfn & ((2ULL << order) - 1)) && pfn + (2ULL << order) <= last)
			order++;
		for (uint64_t i = 0; i < (1ULL << order); i++)
			pages[pfn + i].flags &= ~PG_RESERVED;
		free_locked(pfn, order);
		total_pages += 1ULL << order;
		pfn += 1ULL << order;
	}
}

/* Types whose frames get a struct page (RAM the allocator may own one day). */
static bool is_ram(uint64_t type)
{
	return type == MEM_USABLE || type == MEM_BOOT_RECLAIM || type == MEM_KERNEL;
}

/* Low 1 MiB stays reserved: real-mode trampolines (SMP bring-up) and BIOS data. */
#define LOW_RESERVED 0x100000ULL

static uint64_t array_pa, array_bytes;
uint64_t pmm_reclaimed_pages;
SYSCTL_U64_RO(vm_reclaimed_c, "vm.pmm.reclaimed_pages", &pmm_reclaimed_pages,
	      "bootloader-reclaimable frames given to the allocator");

/* Free [s, e) except the low 1 MiB and the page array. */
static void add_usable(uint64_t s, uint64_t e)
{
	s = MAX(s, LOW_RESERVED);
	uint64_t as = array_pa, ae = array_pa + array_bytes;
	if (e <= s)
		return;
	if (e <= as || s >= ae) {
		add_range(s, e);
		return;
	}
	if (s < as)
		add_range(s, as);
	if (e > ae)
		add_range(ae, e);
}

void pmm_init(void)
{
	const struct bootinfo *bi = &bootinfo;
	for (size_t i = 0; i < bi->nmmap; i++) {
		const struct boot_mmap_entry *m = &bi->mmap[i];
		if (is_ram(m->type) && m->base + m->length > max_phys)
			max_phys = m->base + m->length;
	}
	max_phys = ALIGN_UP(max_phys, PAGE_SIZE);
	npages = max_phys >> PAGE_SHIFT;

	/* The page array goes in the first usable range above 1 MiB that fits.
	 * shortcut: one contiguous array (40 bytes per frame, 320 MiB for 32 GiB),
	 * switch to per-range arrays (FreeBSD vm_phys_seg) for sparse NUMA maps. */
	array_bytes = ALIGN_UP(npages * sizeof(struct page), PAGE_SIZE);
	for (size_t i = 0; i < bi->nmmap && !array_pa; i++) {
		const struct boot_mmap_entry *m = &bi->mmap[i];
		uint64_t s = MAX(m->base, LOW_RESERVED), e = m->base + m->length;
		if (m->type == MEM_USABLE && e > s && e - s >= array_bytes)
			array_pa = s;
	}
	if (!array_pa)
		panic("pmm: no room for the page array (%lu KiB)", array_bytes >> 10);
	pages = phys_to_virt(array_pa);
	memset(pages, 0, array_bytes);
	for (uint64_t i = 0; i < npages; i++)
		pages[i].flags = PG_RESERVED;

	/* Usable RAM only: bootloader-reclaimable memory still holds Limine's
	 * page tables, stack and GDT until pmm_reclaim() runs. */
	for (size_t i = 0; i < bi->nmmap; i++) {
		const struct boot_mmap_entry *m = &bi->mmap[i];
		if (m->type == MEM_USABLE)
			add_usable(m->base, m->base + m->length);
	}
	kprintf("pmm: %lu MiB usable, max phys %p, page array %lu KiB at %p\n",
		(total_pages * PAGE_SIZE) >> 20, (void *)max_phys, array_bytes >> 10, (void *)array_pa);
}

/* After vmm_init() switched to Magia's page tables and cpu.c loaded the
 * kernel's own GDT, nothing references bootloader memory any more
 * (boot_early() copied the responses into `bootinfo`). */
static void pmm_reclaim(void)
{
	uint64_t before = total_pages;
	spin_lock(&pmm_lock);
	for (size_t i = 0; i < bootinfo.nmmap; i++) {
		const struct boot_mmap_entry *m = &bootinfo.mmap[i];
		if (m->type == MEM_BOOT_RECLAIM)
			add_usable(m->base, m->base + m->length);
	}
	spin_unlock(&pmm_lock);
	pmm_reclaimed_pages = total_pages - before;
	kprintf("pmm: reclaimed %lu KiB of bootloader memory, %lu MiB managed\n",
		pmm_reclaimed_pages * 4, (total_pages * PAGE_SIZE) >> 20);
}

static void pmm_sysinit(void) { pmm_init(); }
SYSINIT(pmm, SI_ORDER_VM, pmm_sysinit);
SYSINIT(pmm_reclaim, 20002, pmm_reclaim); /* right after vmm (20001) */
