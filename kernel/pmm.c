/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * pmm.c - physical memory: a binary buddy allocator over the multiboot
 * memory map, with one struct page per 4 KiB frame.
 *
 * Idea: the classic buddy system (Knowlton 1965), as used by Linux's page
 * allocator; FreeBSD's vm_phys uses the same idea. A free block of order k
 * is 2^k pages, aligned to its size; its buddy is at pfn ^ (1 << k).
 * Freeing merges with the buddy while the buddy is a free block of the same
 * order. Free lists are doubly linked through struct page.
 */
#include <magia/linker_set.h>
#include <magia/mm.h>
#include <magia/spinlock.h>
#include <magia/sysctl.h>

extern char __kernel_phys_start[], __kernel_phys_end[];
extern uint32_t boot_mb_info;

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

struct __packed mb_info {
	uint32_t flags, mem_lower, mem_upper, boot_device, cmdline, mods_count, mods_addr;
	uint32_t syms[4];
	uint32_t mmap_length, mmap_addr;
};
struct __packed mb_mmap {
	uint32_t size;
	uint64_t addr, len;
	uint32_t type;
};

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

void pmm_init(uint64_t mbi_phys)
{
	const struct mb_info *mbi = phys_to_virt(mbi_phys);
	if (!(mbi->flags & (1 << 6)))
		panic("pmm: bootloader gave no memory map");
	uint64_t kstart = (uint64_t)__kernel_phys_start;
	uint64_t kend = ALIGN_UP((uint64_t)__kernel_phys_end, PAGE_SIZE);

	/* pass 1: highest usable address (only the first 1 GiB is mapped yet) */
	const uint8_t *p = phys_to_virt(mbi->mmap_addr), *e = p + mbi->mmap_length;
	for (const uint8_t *q = p; q < e; q += ((const struct mb_mmap *)q)->size + 4) {
		const struct mb_mmap *m = (const void *)q;
		if (m->type == 1 && m->addr + m->len > max_phys)
			max_phys = m->addr + m->len;
	}
	if (max_phys > (1ULL << 30))
		max_phys = 1ULL << 30; /* Magia 0.1 manages at most 1 GiB */
	npages = max_phys >> PAGE_SHIFT;

	/* pass 2: place the page array in the first usable range above the kernel */
	uint64_t array_bytes = ALIGN_UP(npages * sizeof(struct page), PAGE_SIZE);
	uint64_t array_pa = 0;
	for (const uint8_t *q = p; q < e && !array_pa; q += ((const struct mb_mmap *)q)->size + 4) {
		const struct mb_mmap *m = (const void *)q;
		uint64_t s = ALIGN_UP(MAX(m->addr, kend), PAGE_SIZE), end = MIN(m->addr + m->len, max_phys);
		if (m->type == 1 && end > s && end - s >= array_bytes)
			array_pa = s;
	}
	if (!array_pa)
		panic("pmm: no room for the page array");
	pages = phys_to_virt(array_pa);
	memset(pages, 0, array_bytes);
	for (uint64_t i = 0; i < npages; i++)
		pages[i].flags = PG_RESERVED;

	/* pass 3: free usable RAM except low 1 MiB, the kernel image, the page
	 * array and the multiboot structures (read by kmain/pmm only, but keep
	 * the first page of them for the debugger). */
	uint64_t mbi_pg = ALIGN_DOWN(mbi_phys, PAGE_SIZE);
	for (const uint8_t *q = p; q < e; q += ((const struct mb_mmap *)q)->size + 4) {
		const struct mb_mmap *m = (const void *)q;
		if (m->type != 1)
			continue;
		uint64_t s = MAX(m->addr, 0x100000ULL), end = MIN(m->addr + m->len, max_phys);
		/* cut out [kstart, kend), [array], [mbi page] by splitting */
		uint64_t cuts[3][2] = { { kstart, kend }, { array_pa, array_pa + array_bytes },
					{ mbi_pg, mbi_pg + PAGE_SIZE } };
		uint64_t pieces[8][2];
		int n = 0;
		pieces[n][0] = s;
		pieces[n][1] = end;
		n++;
		for (int c = 0; c < 3; c++) {
			int m2 = n;
			for (int i = 0; i < m2; i++) {
				uint64_t a = pieces[i][0], b = pieces[i][1];
				if (b <= cuts[c][0] || a >= cuts[c][1])
					continue;
				pieces[i][1] = MAX(a, MIN(b, cuts[c][0]));
				if (b > cuts[c][1] && n < 8) {
					pieces[n][0] = cuts[c][1];
					pieces[n][1] = b;
					n++;
				}
			}
		}
		for (int i = 0; i < n; i++)
			if (pieces[i][1] > pieces[i][0])
				add_range(pieces[i][0], pieces[i][1]);
	}
	kprintf("pmm: %lu MiB usable, %lu pages free, page array %lu KiB at %p\n",
		(total_pages * PAGE_SIZE) >> 20, free_pages, array_bytes >> 10, (void *)array_pa);
}

uint64_t boot_mbi_phys;
static void pmm_sysinit(void) { pmm_init(boot_mbi_phys); }
SYSINIT(pmm, SI_ORDER_VM, pmm_sysinit);
