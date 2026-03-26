#include <kernel/drivers/video/drm/i915/i915.h>

static const struct kernel_drm_i915_id_info g_kernel_drm_i915_known_ids[] = {
    {0x3577u, 2u, "i830"},
    {0x2562u, 2u, "i845g"},
    {0x2582u, 3u, "i915g"},
    {0x2592u, 3u, "i915gm"},
    {0x2772u, 4u, "i945g"},
    {0x27A2u, 4u, "i945gm"},
    {0x29A2u, 4u, "g965"},
    {0x2A02u, 4u, "gm965"},
    {0x2E22u, 4u, "g45"},
    {0x2A42u, 4u, "gm45"},
    {0x0042u, 5u, "ironlake"},
    {0x0102u, 6u, "sandybridge"},
    {0x0112u, 6u, "sandybridge"},
    {0x0122u, 6u, "sandybridge"},
    {0x0152u, 7u, "ivybridge"},
    {0x0162u, 7u, "ivybridge"},
    {0x0402u, 7u, "haswell"},
    {0x0412u, 7u, "haswell"},
    {0x0A16u, 7u, "haswell"},
    {0x0D26u, 7u, "haswell"},
    {0x0F31u, 7u, "valleyview"},
    {0x1602u, 8u, "broadwell"},
    {0x1616u, 8u, "broadwell"},
    {0x1626u, 8u, "broadwell"},
    {0x1902u, 9u, "skylake"},
    {0x1912u, 9u, "skylake"},
    {0x1926u, 9u, "skylake"},
    {0x5912u, 9u, "kabylake"},
    {0x3E92u, 9u, "coffeelake"},
    {0x9BC4u, 9u, "cometlake"},
    {0x8A52u, 11u, "icelake"},
    {0x4E71u, 11u, "jasperlake"},
    {0x9A49u, 12u, "tigerlake"},
};

const struct kernel_drm_i915_id_info *kernel_drm_i915_find_id(uint16_t device_id) {
    size_t i;

    for (i = 0u; i < (sizeof(g_kernel_drm_i915_known_ids) / sizeof(g_kernel_drm_i915_known_ids[0])); ++i) {
        if (g_kernel_drm_i915_known_ids[i].device_id == device_id) {
            return &g_kernel_drm_i915_known_ids[i];
        }
    }
    return 0;
}

int kernel_drm_i915_is_supported_device(uint16_t device_id) {
    return kernel_drm_i915_find_id(device_id) != 0;
}

int kernel_drm_i915_is_modeset_supported_device(uint16_t device_id) {
    const struct kernel_drm_i915_id_info *info = kernel_drm_i915_find_id(device_id);

    return info != 0 && info->gen >= 5u;
}

uint8_t kernel_drm_i915_guess_gen(uint16_t device_id) {
    const struct kernel_drm_i915_id_info *info = kernel_drm_i915_find_id(device_id);

    if (info != 0) {
        return info->gen;
    }

    switch (device_id & 0xFF00u) {
    case 0x0000u:
        return 5u;
    case 0x0100u:
        return 6u;
    case 0x0400u:
    case 0x0A00u:
    case 0x0C00u:
    case 0x0D00u:
    case 0x0F00u:
        return 7u;
    case 0x1600u:
    case 0x2200u:
        return 8u;
    case 0x1900u:
    case 0x5900u:
    case 0x3E00u:
    case 0x9B00u:
        return 9u;
    case 0x8A00u:
    case 0x4E00u:
        return 11u;
    case 0x4500u:
    case 0x9A00u:
        return 12u;
    default:
        break;
    }

    if (device_id == 0x3577u || device_id == 0x2562u || device_id == 0x3582u ||
        device_id == 0x358Eu || device_id == 0x2572u) {
        return 2u;
    }
    if ((device_id >= 0x2582u && device_id <= 0x27AEu) ||
        (device_id >= 0x2972u && device_id <= 0x29D2u)) {
        return 3u;
    }
    if ((device_id >= 0x2A02u && device_id <= 0x2A42u) ||
        (device_id >= 0x2E02u && device_id <= 0x2E92u)) {
        return 4u;
    }

    return 0u;
}

const char *kernel_drm_i915_platform_name(uint16_t device_id) {
    const struct kernel_drm_i915_id_info *info = kernel_drm_i915_find_id(device_id);
    uint8_t gen;

    if (info != 0) {
        return info->platform;
    }

    gen = kernel_drm_i915_guess_gen(device_id);
    switch (gen) {
    case 2u:
        return "gen2";
    case 3u:
        return "gen3";
    case 4u:
        return "gen4";
    case 5u:
        return "gen5";
    case 6u:
        return "gen6";
    case 7u:
        return "gen7";
    case 8u:
        return "gen8";
    case 9u:
        return "gen9";
    case 11u:
        return "gen11";
    case 12u:
        return "gen12";
    default:
        return "unknown";
    }
}
