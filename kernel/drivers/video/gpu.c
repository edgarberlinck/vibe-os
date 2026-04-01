#include <kernel/drivers/video/gpu.h>

size_t kernel_gpu_probe_candidates(struct kernel_gpu_candidate *out, size_t capacity) {
    struct kernel_drm_candidate drm_candidates[8];
    size_t count = kernel_drm_probe_candidates(drm_candidates, 8u);
    size_t i;
    size_t limit = count;

    if (limit > 8u) {
        limit = 8u;
    }
    if (out == 0) {
        return count;
    }

    for (i = 0u; i < limit && i < capacity; ++i) {
        out[i].backend_kind = (enum kernel_gpu_backend_kind)drm_candidates[i].backend_kind;
        out[i].backend_name = drm_candidates[i].backend_name;
        out[i].pci = drm_candidates[i].pci;
        out[i].mmio_base = drm_candidates[i].mmio_base;
        out[i].mmio_size = drm_candidates[i].mmio_size;
        out[i].fb_base = drm_candidates[i].fb_base;
        out[i].fb_size = drm_candidates[i].fb_size;
    }

    return count;
}

void kernel_gpu_log_candidates(void) {
    kernel_drm_log_candidates();
}
