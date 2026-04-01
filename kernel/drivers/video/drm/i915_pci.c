#include <kernel/drivers/pci/pci.h>
#include <kernel/drivers/video/drm/i915/i915.h>

uint32_t kernel_drm_i915_decode_stolen_size(uint8_t gen, uint16_t gmch_ctl) {
    uint32_t gms;

    if (gen >= 9u) {
        gms = (uint32_t)((gmch_ctl >> 8) & 0xFFu);
        if (gms < 0xF0u) {
            return gms * (32u * 1024u * 1024u);
        }
        return ((gms - 0xF0u) * (4u * 1024u * 1024u)) + (4u * 1024u * 1024u);
    }
    if (gen >= 8u) {
        gms = (uint32_t)((gmch_ctl >> 8) & 0xFFu);
        return gms * (32u * 1024u * 1024u);
    }
    if (gen >= 6u) {
        gms = (uint32_t)((gmch_ctl >> 8) & 0x1Fu);
        return gms * (32u * 1024u * 1024u);
    }

    return 0u;
}

int kernel_drm_i915_read_probe_info(const struct kernel_drm_candidate *candidate,
                                    struct kernel_drm_i915_probe_info *info_out) {
    if (candidate == 0 || info_out == 0) {
        return -1;
    }

    info_out->command = kernel_pci_config_read_u16(candidate->pci.bus,
                                                   candidate->pci.slot,
                                                   candidate->pci.function,
                                                   INTEL_I915_PCI_COMMAND);
    info_out->gmch_ctl = kernel_pci_config_read_u16(candidate->pci.bus,
                                                    candidate->pci.slot,
                                                    candidate->pci.function,
                                                    INTEL_I915_GMCH_CTRL);
    info_out->bsm = kernel_pci_config_read_u32(candidate->pci.bus,
                                               candidate->pci.slot,
                                               candidate->pci.function,
                                               INTEL_I915_BSM) & INTEL_I915_BSM_MASK;
    info_out->opregion = kernel_pci_config_read_u32(candidate->pci.bus,
                                                    candidate->pci.slot,
                                                    candidate->pci.function,
                                                    INTEL_I915_OPREGION);
    info_out->gen = kernel_drm_i915_guess_gen(candidate->pci.device_id);
    info_out->platform_name = kernel_drm_i915_platform_name(candidate->pci.device_id);
    info_out->stolen_size = kernel_drm_i915_decode_stolen_size(info_out->gen, info_out->gmch_ctl);
    return 0;
}
