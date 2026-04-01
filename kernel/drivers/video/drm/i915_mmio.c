#include <kernel/drivers/video/drm/i915/i915.h>

static uintptr_t kernel_drm_i915_mmio_ptr(const struct kernel_drm_candidate *candidate,
                                          uint32_t reg) {
    if (candidate == 0 || candidate->mmio_base == (uintptr_t)0) {
        return (uintptr_t)0;
    }
    return candidate->mmio_base + (uintptr_t)reg;
}

uint32_t kernel_drm_i915_mmio_read32(const struct kernel_drm_candidate *candidate, uint32_t reg) {
    volatile uint32_t *ptr = (volatile uint32_t *)(uintptr_t)kernel_drm_i915_mmio_ptr(candidate, reg);

    if (ptr == 0) {
        return 0u;
    }
    return *ptr;
}

void kernel_drm_i915_mmio_write32(const struct kernel_drm_candidate *candidate,
                                  uint32_t reg,
                                  uint32_t value) {
    volatile uint32_t *ptr = (volatile uint32_t *)(uintptr_t)kernel_drm_i915_mmio_ptr(candidate, reg);

    if (ptr == 0) {
        return;
    }
    *ptr = value;
}

int kernel_drm_i915_snapshot_mmio(const struct kernel_drm_candidate *candidate,
                                  struct kernel_drm_i915_mmio_snapshot *snapshot_out) {
    if (candidate == 0 || snapshot_out == 0 || candidate->mmio_base == (uintptr_t)0) {
        return -1;
    }

    snapshot_out->dpll_test = kernel_drm_i915_mmio_read32(candidate, INTEL_I915_MMIO_DPLL_TEST);
    snapshot_out->d_state = kernel_drm_i915_mmio_read32(candidate, INTEL_I915_MMIO_D_STATE);
    snapshot_out->disp_arb_ctl = kernel_drm_i915_mmio_read32(candidate, INTEL_I915_MMIO_DISP_ARB_CTL);
    return 0;
}
