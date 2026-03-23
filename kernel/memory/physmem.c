#include <kernel/bootinfo.h>
#include <kernel/memory/physmem.h>

#define PHYSMEM_FALLBACK_BASE 0x00500000u
#define PHYSMEM_FALLBACK_END 0x00900000u
#define PHYSMEM_MIN_BASE 0x00100000u

static uintptr_t g_physmem_base = PHYSMEM_FALLBACK_BASE;
static uintptr_t g_physmem_end = PHYSMEM_FALLBACK_END;

static uintptr_t align_up_uintptr(uintptr_t value, uintptr_t align) {
    if (align == 0u) {
        return value;
    }
    return (value + align - 1u) & ~(align - 1u);
}

static uintptr_t align_down_uintptr(uintptr_t value, uintptr_t align) {
    if (align == 0u) {
        return value;
    }
    return value & ~(align - 1u);
}

void physmem_init(void) {
    const struct bootinfo *info = (const struct bootinfo *)(uintptr_t)BOOTINFO_ADDR;
    uintptr_t base = PHYSMEM_FALLBACK_BASE;
    uintptr_t end = PHYSMEM_FALLBACK_END;

    if (info->magic == BOOTINFO_MAGIC &&
        info->version == BOOTINFO_VERSION &&
        (info->flags & BOOTINFO_FLAG_MEMINFO_VALID) != 0u) {
        base = (uintptr_t)info->meminfo.largest_base;
        end = (uintptr_t)info->meminfo.largest_end;

        if (base < PHYSMEM_MIN_BASE) {
            base = PHYSMEM_MIN_BASE;
        }
        if (end > (uintptr_t)PHYSMEM_DYNAMIC_CAP_BYTES) {
            end = (uintptr_t)PHYSMEM_DYNAMIC_CAP_BYTES;
        }
        base = align_up_uintptr(base, 0x1000u);
        end = align_down_uintptr(end, 0x1000u);
        if (end <= base) {
            base = PHYSMEM_FALLBACK_BASE;
            end = PHYSMEM_FALLBACK_END;
        }
    }

    g_physmem_base = base;
    g_physmem_end = end;
}

uintptr_t physmem_usable_base(void) {
    return g_physmem_base;
}

uintptr_t physmem_usable_end(void) {
    return g_physmem_end;
}

size_t physmem_usable_size(void) {
    if (g_physmem_end <= g_physmem_base) {
        return 0u;
    }
    return (size_t)(g_physmem_end - g_physmem_base);
}

void *alloc_phys_page(void) {
    return NULL; // placeholder
}

void free_phys_page(void *page) {
    (void)page;
    // placeholder
}
