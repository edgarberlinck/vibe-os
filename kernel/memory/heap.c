#include <kernel/memory/heap.h>
#include <kernel/lock.h>

/* Simple bump allocator for early kernel memory */
static uintptr_t g_heap_start = 0u;
static uintptr_t g_heap_ptr = 0u;
static uintptr_t g_heap_end = 0u;
static size_t g_heap_used = 0u;
static spinlock_t g_heap_lock;
static volatile uint32_t g_heap_initialized = 0u;

void kernel_mm_init(uintptr_t heap_start, size_t heap_size) {
    spinlock_init(&g_heap_lock);
    g_heap_start = heap_start;
    g_heap_ptr = heap_start;
    g_heap_end = heap_start + heap_size;
    g_heap_used = 0u;
    g_heap_initialized = 1u;
}

void *kernel_malloc(size_t size) {
    void *ptr;
    uint32_t flags;

    if (size == 0u) {
        return NULL;
    }
    
    /* Align to 8 bytes */
    if (size & 0x7u) {
        size += 8u - (size & 0x7u);
    }

    if (g_heap_initialized == 0u) {
        return NULL;
    }

    flags = spinlock_lock_irqsave(&g_heap_lock);
    if (g_heap_ptr + size > g_heap_end) {
        spinlock_unlock_irqrestore(&g_heap_lock, flags);
        return NULL;  /* Out of memory */
    }

    ptr = (void *)g_heap_ptr;
    g_heap_ptr += size;
    g_heap_used += size;
    spinlock_unlock_irqrestore(&g_heap_lock, flags);
    return ptr;
}

void *kernel_malloc_aligned(size_t size, size_t align) {
    uintptr_t ptr;
    uintptr_t aligned_ptr;
    size_t padding;
    uint32_t flags;

    if (size == 0u) {
        return NULL;
    }
    if (align == 0u) {
        align = 8u;
    }
    if ((align & (align - 1u)) != 0u) {
        return NULL;
    }
    if (size & 0x7u) {
        size += 8u - (size & 0x7u);
    }

    if (g_heap_initialized == 0u) {
        return NULL;
    }

    flags = spinlock_lock_irqsave(&g_heap_lock);
    ptr = g_heap_ptr;
    aligned_ptr = (ptr + (uintptr_t)(align - 1u)) & ~(uintptr_t)(align - 1u);
    padding = (size_t)(aligned_ptr - ptr);
    if (aligned_ptr + size > g_heap_end) {
        spinlock_unlock_irqrestore(&g_heap_lock, flags);
        return NULL;
    }

    g_heap_ptr = aligned_ptr + size;
    g_heap_used += padding + size;
    spinlock_unlock_irqrestore(&g_heap_lock, flags);
    return (void *)aligned_ptr;
}

void kernel_free(void *ptr) {
    (void)ptr;  /* No-op for bump allocator */
}

uintptr_t kernel_heap_start(void) {
    uintptr_t value;
    uint32_t flags;

    if (g_heap_initialized == 0u) {
        return 0u;
    }
    flags = spinlock_lock_irqsave(&g_heap_lock);
    value = g_heap_start;
    spinlock_unlock_irqrestore(&g_heap_lock, flags);
    return value;
}

uintptr_t kernel_heap_end(void) {
    uintptr_t value;
    uint32_t flags;

    if (g_heap_initialized == 0u) {
        return 0u;
    }
    flags = spinlock_lock_irqsave(&g_heap_lock);
    value = g_heap_end;
    spinlock_unlock_irqrestore(&g_heap_lock, flags);
    return value;
}

size_t kernel_heap_used(void) {
    size_t value;
    uint32_t flags;

    if (g_heap_initialized == 0u) {
        return 0u;
    }
    flags = spinlock_lock_irqsave(&g_heap_lock);
    value = g_heap_used;
    spinlock_unlock_irqrestore(&g_heap_lock, flags);
    return value;
}

size_t kernel_heap_free(void) {
    size_t value;
    uint32_t flags;

    if (g_heap_initialized == 0u) {
        return 0u;
    }
    flags = spinlock_lock_irqsave(&g_heap_lock);
    value = (g_heap_end > g_heap_ptr) ? (g_heap_end - g_heap_ptr) : 0u;
    spinlock_unlock_irqrestore(&g_heap_lock, flags);
    return value;
}

int kernel_heap_is_initialized(void) {
    return g_heap_initialized != 0u;
}
