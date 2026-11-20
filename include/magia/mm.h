/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/* mm.h - physical pages (buddy), paging, kernel memory zones, user windows. */
#ifndef MAGIA_MM_H
#define MAGIA_MM_H

#include <magia/kernel.h>

#define MAX_ORDER 10 /* largest buddy block: 2^10 pages = 4 MiB */

/* Per-physical-page metadata (one per 4 KiB frame), like Linux's struct page
 * or FreeBSD's vm_page. Free buddy blocks are linked through next/prev. */
struct kzone;
struct page {
	uint32_t flags;
	int8_t order;      /* order of the block this page heads           */
	uint8_t _pad;
	uint16_t inuse;    /* slab: objects in use                          */
	struct page *next; /* buddy free list / zone slab list              */
	struct page *prev;
	void *freelist;    /* slab: first free object                       */
	struct kzone *zone;/* slab: owning zone                             */
};

#define PG_RESERVED 0x01u /* never allocatable (BIOS, kernel image)      */
#define PG_FREE     0x02u /* head of a free buddy block                  */
#define PG_SLAB     0x04u /* page backs a kmalloc zone slab              */
#define PG_LARGE    0x08u /* head of a large kmalloc allocation          */
#define PG_USER     0x10u /* mapped into a user window                   */

/* pmm.c: buddy allocator over the multiboot memory map */
void pmm_init(uint64_t mbi_phys);
uint64_t pmm_alloc(int order); /* returns physical address or 0 */
void pmm_free(uint64_t pa, int order);
uint64_t pmm_free_pages(void);
uint64_t pmm_total_pages(void);
uint64_t pmm_max_phys(void);
struct page *pa_to_page(uint64_t pa);
uint64_t page_to_pa(const struct page *pg);
void *page_alloc(int order); /* direct-map address, zeroed */
void page_free(void *va, int order);
void pmm_dump(void);

/* vmm.c: 4-level paging */
#define PTE_P   (1ULL << 0)
#define PTE_W   (1ULL << 1)
#define PTE_U   (1ULL << 2)
#define PTE_PWT (1ULL << 3)
#define PTE_PCD (1ULL << 4)
#define PTE_A   (1ULL << 5)
#define PTE_D   (1ULL << 6)
#define PTE_PS  (1ULL << 7)
#define PTE_G   (1ULL << 8)
#define PTE_NX  (1ULL << 63)
#define PTE_ADDR_MASK 0x000ffffffffff000ULL

void vmm_init(void);
int vmm_map(uint64_t va, uint64_t pa, uint64_t flags);
uint64_t vmm_unmap(uint64_t va); /* returns old physical address or 0 */
bool vmm_translate(uint64_t va, uint64_t *pa, uint64_t *flags);
extern bool nx_enabled;

/* User windows: page ranges below USER_TOP in the single shared address
 * space. Charged to a jail through the Rust accounting hook. */
#define UVM_EXEC   0x1 /* executable (code) mapping, read-only           */
#define UVM_CONTIG 0x2 /* physically contiguous; kernel alias returned   */
int uvm_alloc(int jid, uint64_t npages, int flags, uint64_t *uva, uint64_t *kva);
int uvm_free(int jid, uint64_t uva, uint64_t npages);
bool uvm_range_mapped(uint64_t uva, uint64_t len);
int copyin(void *kdst, uint64_t usrc, size_t n);
int copyout(uint64_t udst, const void *ksrc, size_t n);
void uaccess_begin(void); /* stac when SMAP is on */
void uaccess_end(void);   /* clac when SMAP is on */

/* kmem.c: slab-style zones (FreeBSD UMA / Linux SLUB idea, simplified) */
void kmem_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kmalloc_aligned(size_t size, size_t align);
void kfree(void *p);
void kmem_reclaim(void);
struct kmem_stats {
	uint64_t allocs, frees, bytes_inuse, slab_pages, large_pages;
};
void kmem_get_stats(struct kmem_stats *st);
void kmem_dump(void);

#endif
