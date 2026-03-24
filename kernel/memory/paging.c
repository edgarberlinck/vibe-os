#include <kernel/memory/paging.h>
#include <kernel/drivers/debug/debug.h>

#define PAGING_PAGE_DIR_ENTRIES 1024u
#define PAGING_4MB_PAGE_BYTES 0x00400000u
#define PAGING_PDE_PRESENT 0x001u
#define PAGING_PDE_WRITABLE 0x002u
#define PAGING_PDE_PAGE_SIZE 0x080u
#define PAGING_CR0_PG 0x80000000u
#define PAGING_CR4_PSE 0x00000010u

static uint32_t g_kernel_page_directory[PAGING_PAGE_DIR_ENTRIES] __attribute__((aligned(4096)));
static int g_paging_enabled = 0;

void paging_init(void) {
    uint32_t cr0;
    uint32_t cr4;

    for (uint32_t i = 0; i < PAGING_PAGE_DIR_ENTRIES; ++i) {
        g_kernel_page_directory[i] = (i * PAGING_4MB_PAGE_BYTES) |
                                     PAGING_PDE_PRESENT |
                                     PAGING_PDE_WRITABLE |
                                     PAGING_PDE_PAGE_SIZE;
    }

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= PAGING_CR4_PSE;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    __asm__ volatile("mov %0, %%cr3" : : "r"((uint32_t)(uintptr_t)g_kernel_page_directory) : "memory");

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= PAGING_CR0_PG;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    g_paging_enabled = 1;
    kernel_debug_printf("paging: enabled cr3=%x span_mb=%d\n",
                        (uint32_t)(uintptr_t)g_kernel_page_directory,
                        (int)(PAGING_PAGE_DIR_ENTRIES * 4u));
}

int paging_is_enabled(void) {
    return g_paging_enabled;
}

uintptr_t paging_page_directory_phys(void) {
    return (uintptr_t)g_kernel_page_directory;
}
