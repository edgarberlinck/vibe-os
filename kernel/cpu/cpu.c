#include <kernel/cpu/cpu.h>
#include <kernel/apic.h>
#include <kernel/drivers/debug/debug.h>
#include <stddef.h>

struct mp_floating_pointer {
    char signature[4];
    uint32_t config_table;
    uint8_t length;
    uint8_t spec_rev;
    uint8_t checksum;
    uint8_t feature1;
    uint8_t feature2;
    uint8_t feature3;
    uint8_t feature4;
    uint8_t feature5;
} __attribute__((packed));

struct mp_config_table_header {
    char signature[4];
    uint16_t base_length;
    uint8_t spec_rev;
    uint8_t checksum;
    char oem_id[8];
    char product_id[12];
    uint32_t oem_table;
    uint16_t oem_table_size;
    uint16_t entry_count;
    uint32_t local_apic_addr;
    uint16_t extended_length;
    uint8_t extended_checksum;
    uint8_t reserved;
} __attribute__((packed));

struct mp_processor_entry {
    uint8_t entry_type;
    uint8_t apic_id;
    uint8_t apic_version;
    uint8_t cpu_flags;
    uint32_t cpu_signature;
    uint32_t feature_flags;
    uint32_t reserved0;
    uint32_t reserved1;
} __attribute__((packed));

static struct kernel_cpu_topology g_cpu_topology = {
    1u, 0u, 0u, 0u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, "unknown"
};
static struct kernel_cpu_state g_cpu_states[32];
static uint32_t g_cpu_has_pat = 0u;
static uint32_t g_cpu_has_sse2 = 0u;
static uint32_t g_cpu_sse_enabled = 0u;

static int cpu_has_cpuid(void) {
    uint32_t before;
    uint32_t after;

    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "movl %0, %1\n\t"
        "xorl $0x00200000, %1\n\t"
        "pushl %1\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %1\n\t"
        "pushl %0\n\t"
        "popfl\n\t"
        : "=&r"(before), "=&r"(after)
        :
        : "cc", "memory");

    return ((before ^ after) & 0x00200000u) != 0u;
}

static void cpu_cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;

    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(0u)
                     : "cc");

    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

static void cpu_try_enable_sse(void) {
    uint32_t cr0;
    uint32_t cr4;

    if (!g_cpu_has_sse2) {
        return;
    }

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2); /* EM */
    cr0 |= (1u << 1);  /* MP */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1u << 9);  /* OSFXSR */
    cr4 |= (1u << 10); /* OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    __asm__ volatile("fninit");
    g_cpu_sse_enabled = 1u;
}

static uint8_t cpu_checksum_ok(const void *ptr, size_t len) {
    const uint8_t *bytes = (const uint8_t *)ptr;
    uint8_t sum = 0u;

    for (size_t i = 0; i < len; ++i) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return sum == 0u;
}

static uint16_t cpu_read_lowmem_u16(uint32_t addr) {
    uint16_t value;
    __asm__ volatile("movw (%1), %0"
                     : "=r"(value)
                     : "r"((uintptr_t)addr)
                     : "memory");
    return value;
}

static const struct mp_floating_pointer *cpu_scan_mp_range(uintptr_t start, uintptr_t end) {
    for (uintptr_t addr = start; addr + sizeof(struct mp_floating_pointer) <= end; addr += 16u) {
        const struct mp_floating_pointer *mp = (const struct mp_floating_pointer *)addr;
        if (mp->signature[0] != '_' ||
            mp->signature[1] != 'M' ||
            mp->signature[2] != 'P' ||
            mp->signature[3] != '_') {
            continue;
        }
        if (mp->length == 0u) {
            continue;
        }
        if (cpu_checksum_ok(mp, (size_t)mp->length * 16u)) {
            return mp;
        }
    }
    return NULL;
}

static uint32_t cpu_detect_from_mp_table(uint32_t *boot_cpu_id_out) {
    uint16_t ebda_seg = cpu_read_lowmem_u16(0x40Eu);
    uintptr_t ebda_addr = ebda_seg << 4;
    uintptr_t base_kb = (uintptr_t)cpu_read_lowmem_u16(0x413u);
    const struct mp_floating_pointer *mp = NULL;

    if (ebda_addr >= 0x80000u && ebda_addr < 0xA0000u) {
        mp = cpu_scan_mp_range(ebda_addr, ebda_addr + 1024u);
    }
    if (!mp && base_kb >= 1u) {
        uintptr_t top = base_kb * 1024u;
        if (top >= 1024u) {
            mp = cpu_scan_mp_range(top - 1024u, top);
        }
    }
    if (!mp) {
        mp = cpu_scan_mp_range(0xF0000u, 0x100000u);
    }
    if (!mp || mp->config_table == 0u) {
        return 0u;
    }

    const struct mp_config_table_header *cfg = (const struct mp_config_table_header *)(uintptr_t)mp->config_table;
    if (cfg->signature[0] != 'P' ||
        cfg->signature[1] != 'C' ||
        cfg->signature[2] != 'M' ||
        cfg->signature[3] != 'P') {
        return 0u;
    }
    if (cfg->base_length < sizeof(*cfg) || !cpu_checksum_ok(cfg, cfg->base_length)) {
        return 0u;
    }

    const uint8_t *ptr = (const uint8_t *)(cfg + 1);
    const uint8_t *end = ((const uint8_t *)cfg) + cfg->base_length;
    uint32_t cpu_count = 0u;
    uint32_t boot_cpu_id = 0u;

    while (ptr < end) {
        uint8_t entry_type = *ptr;
        if (entry_type == 0u) {
            const struct mp_processor_entry *cpu = (const struct mp_processor_entry *)ptr;
            if ((cpu->cpu_flags & 0x01u) != 0u) {
                if (cpu_count < 32u) {
                    g_cpu_states[cpu_count].logical_index = cpu_count;
                    g_cpu_states[cpu_count].apic_id = cpu->apic_id;
                    g_cpu_states[cpu_count].started = 0u;
                    g_cpu_states[cpu_count].is_boot_cpu = ((cpu->cpu_flags & 0x02u) != 0u) ? 1u : 0u;
                }
                if ((cpu->cpu_flags & 0x02u) != 0u) {
                    boot_cpu_id = cpu->apic_id;
                }
                ++cpu_count;
            }
            ptr += sizeof(*cpu);
            continue;
        }
        if (entry_type == 1u || entry_type == 2u || entry_type == 3u || entry_type == 4u) {
            ptr += 8u;
            continue;
        }
        break;
    }

    if (boot_cpu_id_out) {
        *boot_cpu_id_out = boot_cpu_id;
    }
    if (cpu_count > 1u) {
        for (uint32_t i = 0; i < cpu_count && i < 32u; ++i) {
            if (g_cpu_states[i].is_boot_cpu) {
                if (i != 0u) {
                    struct kernel_cpu_state tmp = g_cpu_states[0];
                    g_cpu_states[0] = g_cpu_states[i];
                    g_cpu_states[i] = tmp;
                    g_cpu_states[0].logical_index = 0u;
                    g_cpu_states[i].logical_index = i;
                }
                break;
            }
        }
    }
    return cpu_count;
}

void cpu_init(void) {
    uint32_t eax = 0u;
    uint32_t ebx = 0u;
    uint32_t ecx = 0u;
    uint32_t edx = 0u;
    uint32_t boot_cpu_id = 0u;
    uint32_t mp_cpu_count = 0u;

    g_cpu_topology.cpu_count = 1u;
    g_cpu_topology.boot_cpu_id = 0u;
    g_cpu_topology.apic_supported = 0u;
    g_cpu_topology.cpuid_supported = cpu_has_cpuid() ? 1u : 0u;
    g_cpu_topology.cpuid_logical_cpus = 1u;
    g_cpu_topology.cpuid_core_cpus = 1u;
    g_cpu_topology.cpuid_family = 0u;
    g_cpu_topology.cpuid_model = 0u;
    g_cpu_topology.cpuid_stepping = 0u;
    g_cpu_topology.mp_table_present = 0u;
    g_cpu_topology.synthetic_apic_map = 0u;
    g_cpu_topology.vendor[0] = 'u';
    g_cpu_topology.vendor[1] = 'n';
    g_cpu_topology.vendor[2] = 'k';
    g_cpu_topology.vendor[3] = 'n';
    g_cpu_topology.vendor[4] = 'o';
    g_cpu_topology.vendor[5] = 'w';
    g_cpu_topology.vendor[6] = 'n';
    g_cpu_topology.vendor[7] = '\0';
    for (uint32_t i = 0; i < 32u; ++i) {
        g_cpu_states[i].logical_index = i;
        g_cpu_states[i].apic_id = 0u;
        g_cpu_states[i].started = 0u;
        g_cpu_states[i].is_boot_cpu = 0u;
    }

    if (g_cpu_topology.cpuid_supported) {
        cpu_cpuid(0u, &eax, &ebx, &ecx, &edx);
        ((uint32_t *)g_cpu_topology.vendor)[0] = ebx;
        ((uint32_t *)g_cpu_topology.vendor)[1] = edx;
        ((uint32_t *)g_cpu_topology.vendor)[2] = ecx;
        g_cpu_topology.vendor[12] = '\0';

        if (eax >= 1u) {
            uint32_t base_family;
            uint32_t base_model;
            uint32_t ext_family;
            uint32_t ext_model;

            cpu_cpuid(1u, &eax, &ebx, &ecx, &edx);
            g_cpu_topology.apic_supported = ((edx & (1u << 9)) != 0u) ? 1u : 0u;
            g_cpu_topology.cpuid_logical_cpus = (ebx >> 16) & 0xFFu;
            g_cpu_has_pat = ((edx >> 16) & 0x1u);
            g_cpu_has_sse2 = ((edx >> 26) & 0x1u);
            base_family = (eax >> 8) & 0xFu;
            base_model = (eax >> 4) & 0xFu;
            ext_family = (eax >> 20) & 0xFFu;
            ext_model = (eax >> 16) & 0xFu;
            g_cpu_topology.cpuid_stepping = eax & 0xFu;
            g_cpu_topology.cpuid_family = base_family;
            if (base_family == 0xFu) {
                g_cpu_topology.cpuid_family += ext_family;
            }
            g_cpu_topology.cpuid_model = base_model;
            if (base_family == 0x6u || base_family == 0xFu) {
                g_cpu_topology.cpuid_model |= (ext_model << 4);
            }
            if (g_cpu_topology.cpuid_logical_cpus == 0u) {
                g_cpu_topology.cpuid_logical_cpus = 1u;
            }
            g_cpu_topology.boot_cpu_id = (ebx >> 24) & 0xFFu;
        }

        cpu_cpuid(0u, &eax, &ebx, &ecx, &edx);
        if (eax >= 4u &&
            g_cpu_topology.vendor[0] == 'G' &&
            g_cpu_topology.vendor[1] == 'e' &&
            g_cpu_topology.vendor[2] == 'n') {
            cpu_cpuid(4u, &eax, &ebx, &ecx, &edx);
            if ((eax & 0x1Fu) != 0u) {
                uint32_t cores = ((eax >> 26) & 0x3Fu) + 1u;
                if (cores > g_cpu_topology.cpuid_core_cpus) {
                    g_cpu_topology.cpuid_core_cpus = cores;
                }
            }
        }

        if (g_cpu_topology.cpuid_logical_cpus > g_cpu_topology.cpuid_core_cpus) {
            g_cpu_topology.cpuid_core_cpus = g_cpu_topology.cpuid_logical_cpus;
        }
    }

    cpu_try_enable_sse();

    mp_cpu_count = cpu_detect_from_mp_table(&boot_cpu_id);
    if (mp_cpu_count > 1u) {
        g_cpu_topology.cpu_count = mp_cpu_count;
        g_cpu_topology.boot_cpu_id = boot_cpu_id;
        g_cpu_topology.mp_table_present = 1u;
    } else if (g_cpu_topology.cpuid_core_cpus > 1u) {
        g_cpu_topology.cpu_count = g_cpu_topology.cpuid_core_cpus;
        if (g_cpu_topology.apic_supported) {
            g_cpu_topology.synthetic_apic_map = 1u;
        }
        if (mp_cpu_count == 1u) {
            kernel_debug_puts("cpu: MP table exposed only one processor entry; promoting CPUID logical topology for SMP bring-up\n");
        } else {
            kernel_debug_puts("cpu: no usable MP table; promoting CPUID logical topology for SMP bring-up\n");
        }
    }
    if (g_cpu_topology.cpu_count > 32u) {
        g_cpu_topology.cpu_count = 32u;
    }
    g_cpu_states[0].started = 1u;
    g_cpu_states[0].apic_id = g_cpu_topology.boot_cpu_id;
    g_cpu_states[0].is_boot_cpu = 1u;
    for (uint32_t i = 1; i < g_cpu_topology.cpu_count; ++i) {
        g_cpu_states[i].started = 0u;
        g_cpu_states[i].is_boot_cpu = 0u;
        if (!g_cpu_topology.mp_table_present) {
            g_cpu_states[i].apic_id = i;
        }
    }

    kernel_debug_printf("cpu: vendor=%s cpuid=%d family=%d model=%d stepping=%d apic=%d logical=%d cores=%d detected=%d bsp=%d mp=%d synth=%d\n",
                        g_cpu_topology.vendor,
                        (int)g_cpu_topology.cpuid_supported,
                        (int)g_cpu_topology.cpuid_family,
                        (int)g_cpu_topology.cpuid_model,
                        (int)g_cpu_topology.cpuid_stepping,
                        (int)g_cpu_topology.apic_supported,
                        (int)g_cpu_topology.cpuid_logical_cpus,
                        (int)g_cpu_topology.cpuid_core_cpus,
                        (int)g_cpu_topology.cpu_count,
                        (int)g_cpu_topology.boot_cpu_id,
                        (int)g_cpu_topology.mp_table_present,
                        (int)g_cpu_topology.synthetic_apic_map);
    if (g_cpu_topology.cpu_count > 1u && g_cpu_topology.synthetic_apic_map) {
        kernel_debug_puts("cpu: SMP bring-up will use a synthetic APIC-id map derived from CPUID logical topology\n");
    } else if (g_cpu_topology.cpu_count > 1u) {
        kernel_debug_puts("cpu: MP table detected; LAPIC/SMP bring-up allowed\n");
    }
    kernel_debug_printf("cpu: pat=%d sse2=%d sse=%d\n",
                        (int)g_cpu_has_pat,
                        (int)g_cpu_has_sse2,
                        (int)g_cpu_sse_enabled);
}

void gdt_init(void) {
    /* Flat GDT loaded by bootloader; stub retained for clarity and future work. */
}

const struct kernel_cpu_topology *kernel_cpu_topology(void) {
    return &g_cpu_topology;
}

const struct kernel_cpu_state *kernel_cpu_state(uint32_t index) {
    if (index >= g_cpu_topology.cpu_count) {
        return NULL;
    }
    return &g_cpu_states[index];
}

uint32_t kernel_cpu_count(void) {
    return g_cpu_topology.cpu_count;
}

uint32_t kernel_cpu_boot_id(void) {
    return g_cpu_topology.boot_cpu_id;
}

uint32_t kernel_cpu_index(void) {
    if (local_apic_enabled()) {
        uint32_t apic_id = local_apic_id();
        for (uint32_t i = 0; i < g_cpu_topology.cpu_count; ++i) {
            if (g_cpu_states[i].apic_id == apic_id) {
                return i;
            }
        }
    }
    return 0u;
}

int kernel_cpu_is_smp_capable(void) {
    return g_cpu_topology.cpu_count > 1u &&
           g_cpu_topology.apic_supported &&
           (g_cpu_topology.mp_table_present || g_cpu_topology.synthetic_apic_map);
}

int kernel_cpu_mark_started(uint32_t apic_id) {
    for (uint32_t i = 0; i < g_cpu_topology.cpu_count; ++i) {
        if (g_cpu_states[i].apic_id == apic_id) {
            g_cpu_states[i].started = 1u;
            return (int)i;
        }
    }
    return -1;
}

int kernel_cpu_has_pat(void) {
    return g_cpu_has_pat != 0u;
}

int kernel_cpu_has_sse2(void) {
    return g_cpu_has_sse2 != 0u;
}

int kernel_cpu_sse_enabled(void) {
    return g_cpu_sse_enabled != 0u;
}
