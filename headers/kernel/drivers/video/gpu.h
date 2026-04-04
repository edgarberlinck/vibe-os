#ifndef KERNEL_DRIVERS_VIDEO_GPU_H
#define KERNEL_DRIVERS_VIDEO_GPU_H

#include <stddef.h>
#include <stdint.h>
#include <kernel/drivers/pci/pci.h>
#include <kernel/drivers/video/drm/drm.h>

enum kernel_gpu_backend_kind {
    KERNEL_GPU_BACKEND_NONE = 0,
    KERNEL_GPU_BACKEND_BGA,
    KERNEL_GPU_BACKEND_I915,
    KERNEL_GPU_BACKEND_RADEON,
    KERNEL_GPU_BACKEND_NOUVEAU,
    KERNEL_GPU_BACKEND_UNKNOWN
};

struct kernel_gpu_candidate {
    enum kernel_gpu_backend_kind backend_kind;
    const char *backend_name;
    struct kernel_pci_device_info pci;
    uintptr_t mmio_base;
    size_t mmio_size;
    uintptr_t fb_base;
    size_t fb_size;
};

struct kernel_gpu_backend_ops {
    enum kernel_gpu_backend_kind kind;
    const char *name;
    int (*probe)(const struct kernel_gpu_candidate *candidate);
};

size_t kernel_gpu_probe_candidates(struct kernel_gpu_candidate *out, size_t capacity);
void kernel_gpu_log_candidates(void);

#endif
