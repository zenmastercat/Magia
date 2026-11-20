/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * boot.c - Limine boot protocol requests and the hand-off to kmain.
 *
 * Limine (base revision 6) loads magia.elf, a static PIE linked at
 * KERNEL_VMA: it applies the R_X86_64_RELATIVE relocations, slides the
 * image by a random multiple of 4 KiB ("kaslr: yes"), maps all RAM at a
 * randomised higher-half direct map (HHDM) and fills in the responses
 * below. Magia keeps Limine's HHDM offset as its own direct-map base.
 *
 * Everything Limine hands over lives in bootloader-reclaimable memory, so
 * boot_early() copies what the kernel needs into `bootinfo` before any
 * other code runs; pmm.c frees that memory once Magia's own page tables
 * are active.
 */
#include <magia/boot.h>
#include <limine.h>

uint64_t dmap_base;
uint64_t kernel_virt_base = KERNEL_VMA;
uint64_t kernel_phys_base;
struct bootinfo bootinfo;

#define REQ __attribute__((used, section(".limine_requests"))) static volatile

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start[] = LIMINE_REQUESTS_START_MARKER;

REQ uint64_t base_revision[] = LIMINE_BASE_REVISION(6);
REQ struct limine_hhdm_request hhdm_req = { .id = LIMINE_HHDM_REQUEST_ID };
REQ struct limine_memmap_request memmap_req = { .id = LIMINE_MEMMAP_REQUEST_ID };
REQ struct limine_executable_address_request kaddr_req = { .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID };
REQ struct limine_executable_cmdline_request cmdline_req = { .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID };
REQ struct limine_framebuffer_request fb_req = { .id = LIMINE_FRAMEBUFFER_REQUEST_ID };
REQ struct limine_rsdp_request rsdp_req = { .id = LIMINE_RSDP_REQUEST_ID };
REQ struct limine_firmware_type_request fw_req = { .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID };
REQ struct limine_entropy_request entropy_req = { .id = LIMINE_ENTROPY_REQUEST_ID,
						  .value_count = BOOT_ENTROPY_MAX };
/* vmm.c builds 4-level tables: never let Limine turn on LA57. */
REQ struct limine_paging_mode_request paging_req = { .id = LIMINE_PAGING_MODE_REQUEST_ID,
						     .mode = LIMINE_PAGING_MODE_X86_64_4LVL,
						     .max_mode = LIMINE_PAGING_MODE_X86_64_4LVL,
						     .min_mode = LIMINE_PAGING_MODE_X86_64_4LVL };

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] = LIMINE_REQUESTS_END_MARKER;

/* Too early for kprintf: write straight to COM1, then stop. */
static __noreturn void early_fail(const char *msg)
{
	for (const char *p = "Magia: boot failed: "; *p; p++)
		__asm__ volatile("outb %0, %1" ::"a"(*p), "Nd"((uint16_t)0x3f8));
	for (; *msg; msg++)
		__asm__ volatile("outb %0, %1" ::"a"(*msg), "Nd"((uint16_t)0x3f8));
	for (;;)
		__asm__ volatile("cli; hlt");
}

void boot_early(void)
{
	if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision))
		early_fail("bootloader does not support Limine base revision 6\r\n");
	if (!hhdm_req.response || !memmap_req.response || !kaddr_req.response)
		early_fail("missing HHDM, memory map or executable address\r\n");
	if (paging_req.response && paging_req.response->mode != LIMINE_PAGING_MODE_X86_64_4LVL)
		early_fail("bootloader did not use 4-level paging\r\n");

	struct bootinfo *bi = &bootinfo;
	bi->base_revision = LIMINE_LOADED_BASE_REVISION_VALID(base_revision)
				    ? LIMINE_LOADED_BASE_REVISION(base_revision)
				    : 6;
	dmap_base = hhdm_req.response->offset;
	kernel_phys_base = bi->kernel_phys = kaddr_req.response->physical_base;
	kernel_virt_base = bi->kernel_virt = kaddr_req.response->virtual_base;
	bi->kernel_slide = bi->kernel_virt - KERNEL_VMA;

	const struct limine_memmap_response *mm = memmap_req.response;
	if (mm->entry_count > BOOT_MMAP_MAX)
		early_fail("memory map has too many entries\r\n");
	for (uint64_t i = 0; i < mm->entry_count; i++) {
		bi->mmap[i].base = mm->entries[i]->base;
		bi->mmap[i].length = mm->entries[i]->length;
		bi->mmap[i].type = mm->entries[i]->type;
	}
	bi->nmmap = mm->entry_count;

	if (cmdline_req.response && cmdline_req.response->cmdline)
		strlcpy(bi->cmdline, cmdline_req.response->cmdline, sizeof(bi->cmdline));
	if (fw_req.response)
		bi->firmware = fw_req.response->firmware_type;
	if (rsdp_req.response && rsdp_req.response->address) {
		uint64_t a = (uint64_t)rsdp_req.response->address;
		bi->rsdp_pa = a >= dmap_base ? a - dmap_base : a; /* HHDM-virtual since revision 4 */
	}
	if (fb_req.response && fb_req.response->framebuffer_count > 0) {
		const struct limine_framebuffer *f = fb_req.response->framebuffers[0];
		if (f->memory_model == LIMINE_FRAMEBUFFER_RGB && f->bpp == 32) {
			bi->fb = (struct boot_fb){
				.pa = (uint64_t)f->address - dmap_base,
				.width = f->width, .height = f->height, .pitch = f->pitch, .bpp = f->bpp,
				.red_shift = f->red_mask_shift, .green_shift = f->green_mask_shift,
				.blue_shift = f->blue_mask_shift, .red_size = f->red_mask_size,
				.green_size = f->green_mask_size, .blue_size = f->blue_mask_size,
			};
		}
	}
	if (entropy_req.response) {
		const struct limine_entropy_response *e = entropy_req.response;
		size_t n = MIN((size_t)e->value_count, (size_t)BOOT_ENTROPY_MAX);
		for (size_t i = 0; i < n && e->values; i++)
			bi->entropy[i] = e->values[i];
		bi->nentropy = e->values ? n : 0;
	}
}
