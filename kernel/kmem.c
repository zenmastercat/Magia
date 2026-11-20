/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * kmem.c - kmalloc on top of the buddy allocator.
 *
 * Idea: slab-style object caches (Bonwick's slab allocator; FreeBSD UMA
 * zones, Linux SLUB), much simplified: one zone per power-of-two size from
 * 16 to 2048 bytes, each slab is one page whose struct page holds the free
 * list and the in-use count. Objects are laid out from the page start, so a
 * 2^k object is 2^k-aligned. Larger requests take whole buddy blocks.
 * A freed object is tagged with a poison word; freeing a tagged object
 * that is already on its slab's free list panics (double free).
 */
#include <magia/linker_set.h>
#include <magia/mm.h>
#include <magia/spinlock.h>
#include <magia/sysctl.h>

#define NZONES 8 /* 16 .. 2048 */
#define FREE_POISON 0x6b6d656d66726565ULL /* "kmemfree" */

struct kzone {
	uint32_t size;
	uint32_t per_slab;
	struct page *partial; /* slabs with at least one free object */
	uint64_t slabs, empty;
};

static struct kzone zones[NZONES];
static spinlock_t kmem_lock = SPINLOCK_INIT("kmem");
static struct kmem_stats st;

SYSCTL_U64_RO(vm_kmem_bytes, "vm.kmem.bytes_inuse", &st.bytes_inuse, "bytes handed out by kmalloc");
SYSCTL_U64_RO(vm_kmem_slab_pages, "vm.kmem.slab_pages", &st.slab_pages, "pages backing kmalloc zones");

void kmem_init(void)
{
	for (int i = 0; i < NZONES; i++) {
		zones[i].size = 16u << i;
		zones[i].per_slab = (uint32_t)(PAGE_SIZE / zones[i].size);
	}
}

static int zone_index(size_t size)
{
	for (int i = 0; i < NZONES; i++)
		if (size <= (16u << i))
			return i;
	return -1;
}

static void partial_push(struct kzone *z, struct page *pg)
{
	pg->prev = NULL;
	pg->next = z->partial;
	if (pg->next)
		pg->next->prev = pg;
	z->partial = pg;
}

static void partial_remove(struct kzone *z, struct page *pg)
{
	if (pg->prev)
		pg->prev->next = pg->next;
	else
		z->partial = pg->next;
	if (pg->next)
		pg->next->prev = pg->prev;
	pg->next = pg->prev = NULL;
}

static void *zone_alloc(struct kzone *z)
{
	struct page *pg = z->partial;
	if (!pg) {
		uint64_t pa = pmm_alloc(0);
		if (!pa)
			return NULL;
		pg = pa_to_page(pa);
		pg->flags |= PG_SLAB;
		pg->zone = z;
		pg->inuse = 0;
		uint8_t *base = phys_to_virt(pa);
		pg->freelist = NULL;
		for (int i = (int)z->per_slab - 1; i >= 0; i--) {
			void **obj = (void **)(base + (size_t)i * z->size);
			obj[0] = pg->freelist;
			obj[1] = (void *)FREE_POISON;
			pg->freelist = obj;
		}
		partial_push(z, pg);
		z->slabs++;
		z->empty++;
		st.slab_pages++;
	}
	void **obj = pg->freelist;
	pg->freelist = obj[0];
	obj[1] = NULL;
	if (pg->inuse++ == 0)
		z->empty--;
	if (!pg->freelist)
		partial_remove(z, pg);
	return obj;
}

static void zone_free(struct kzone *z, struct page *pg, void *p)
{
	uint64_t off = (uint64_t)p & (PAGE_SIZE - 1);
	if (off % z->size)
		panic("kfree: %p is not the start of a %u-byte object", p, z->size);
	void **obj = p;
	if (obj[1] == (void *)FREE_POISON)
		for (void **q = pg->freelist; q; q = q[0])
			if (q == obj)
				panic("kfree: double free of %p (%u-byte zone)", p, z->size);
	bool was_full = pg->freelist == NULL;
	obj[0] = pg->freelist;
	obj[1] = (void *)FREE_POISON;
	pg->freelist = obj;
	if (was_full)
		partial_push(z, pg);
	if (--pg->inuse == 0) {
		z->empty++;
		if (z->empty > 1) { /* keep one empty slab cached per zone */
			partial_remove(z, pg);
			z->empty--;
			z->slabs--;
			st.slab_pages--;
			pg->zone = NULL;
			pmm_free(page_to_pa(pg), 0);
		}
	}
}

static int order_for(size_t bytes)
{
	int o = 0;
	while ((PAGE_SIZE << o) < bytes)
		o++;
	return o;
}

void *kmalloc_aligned(size_t size, size_t align)
{
	if (size == 0)
		size = 1;
	if (align < 16)
		align = 16;
	size_t need = size > align ? size : align;
	void *p = NULL;
	spin_lock(&kmem_lock);
	int zi = zone_index(need);
	if (zi >= 0) {
		p = zone_alloc(&zones[zi]);
		if (p)
			st.bytes_inuse += zones[zi].size;
	} else {
		int o = order_for(need);
		if (o <= MAX_ORDER) {
			uint64_t pa = pmm_alloc(o);
			if (pa) {
				struct page *pg = pa_to_page(pa);
				pg->flags |= PG_LARGE;
				pg->order = (int8_t)o;
				p = phys_to_virt(pa);
				st.large_pages += 1ULL << o;
				st.bytes_inuse += PAGE_SIZE << o;
			}
		}
	}
	if (p)
		st.allocs++;
	spin_unlock(&kmem_lock);
	return p;
}

void *kmalloc(size_t size) { return kmalloc_aligned(size, 16); }

void *kzalloc(size_t size)
{
	void *p = kmalloc(size);
	if (p)
		memset(p, 0, size);
	return p;
}

void kfree(void *p)
{
	if (!p)
		return;
	uint64_t va = (uint64_t)p;
	if (va < PHYS_MAP_BASE || va >= KERNEL_VMA)
		panic("kfree: %p is not a kmalloc pointer", p);
	struct page *pg = pa_to_page(virt_to_phys(p));
	if (!pg)
		panic("kfree: %p outside RAM", p);
	spin_lock(&kmem_lock);
	if (pg->flags & PG_SLAB) {
		st.bytes_inuse -= pg->zone->size;
		zone_free(pg->zone, pg, p);
	} else if (pg->flags & PG_LARGE) {
		if (va & (PAGE_SIZE - 1))
			panic("kfree: %p is inside a large allocation", p);
		int o = pg->order;
		st.large_pages -= 1ULL << o;
		st.bytes_inuse -= PAGE_SIZE << o;
		pmm_free(page_to_pa(pg), o);
	} else {
		panic("kfree: %p was not allocated by kmalloc", p);
	}
	st.frees++;
	spin_unlock(&kmem_lock);
}

/* Give every cached empty slab back to the page allocator. */
void kmem_reclaim(void)
{
	spin_lock(&kmem_lock);
	for (int i = 0; i < NZONES; i++) {
		struct kzone *z = &zones[i];
		struct page *pg = z->partial;
		while (pg) {
			struct page *next = pg->next;
			if (pg->inuse == 0) {
				partial_remove(z, pg);
				z->empty--;
				z->slabs--;
				st.slab_pages--;
				pg->zone = NULL;
				pmm_free(page_to_pa(pg), 0);
			}
			pg = next;
		}
	}
	spin_unlock(&kmem_lock);
}

void kmem_get_stats(struct kmem_stats *out)
{
	spin_lock(&kmem_lock);
	*out = st;
	spin_unlock(&kmem_lock);
}

void kmem_dump(void)
{
	struct kmem_stats s;
	kmem_get_stats(&s);
	kprintf("kmem: %lu allocs, %lu frees, %lu bytes in use, %lu slab pages, %lu large pages\n",
		s.allocs, s.frees, s.bytes_inuse, s.slab_pages, s.large_pages);
	for (int i = 0; i < NZONES; i++)
		kprintf("  zone %4u: %lu slabs\n", zones[i].size, zones[i].slabs);
}

static void kmem_sysinit(void) { kmem_init(); }
SYSINIT(kmem, SI_ORDER_KMEM, kmem_sysinit);
