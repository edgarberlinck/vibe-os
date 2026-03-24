#include <kernel/drivers/video/video.h>
#include <kernel/drivers/debug/debug.h>
#include <kernel/drivers/input/input.h>
#include <kernel/memory/heap.h>
#include <kernel/hal/io.h>
#include <stddef.h>

/* internal state */
static struct video_mode g_mode;
static volatile uint8_t *g_fb = NULL;
static uint8_t *g_backbuf = NULL;
static size_t g_buf_size = 0;
static uint16_t g_fb_pitch = 0u;
static uint8_t g_fb_bpp = 0u;
static uint8_t g_palette[256u * 3u];
static int g_palette_ready = 0;
static uint8_t *g_graphics_backbuf = NULL;
static size_t g_graphics_backbuf_capacity = 0u;
static int g_graphics_enabled = 0;
static int g_graphics_bga = 0;
static int g_graphics_boot_lfb = 0;
static int g_video_initialized = 0;
static uint8_t g_early_graphics_backbuf[1024u * 768u];

#define GRAPHICS_DEFAULT_WIDTH 640u
#define GRAPHICS_DEFAULT_HEIGHT 480u
#define GRAPHICS_MAX_WIDTH 1920u
#define GRAPHICS_MAX_HEIGHT 1080u
#define GRAPHICS_BPP 8u
#define GRAPHICS_MIN_FB_ADDR 0x00100000u
#define GRAPHICS_BANK_SIZE 65536u
#define GRAPHICS_MAX_PITCH 2048u
#define GRAPHICS_MAX_BYTES ((size_t)GRAPHICS_MAX_PITCH * (size_t)GRAPHICS_MAX_HEIGHT)

#define BGA_INDEX_PORT 0x01CEu
#define BGA_DATA_PORT 0x01CFu
#define BGA_INDEX_ID 0u
#define BGA_INDEX_XRES 1u
#define BGA_INDEX_YRES 2u
#define BGA_INDEX_BPP 3u
#define BGA_INDEX_ENABLE 4u
#define BGA_INDEX_BANK 5u
#define BGA_ID0 0xB0C0u
#define BGA_ID5 0xB0C5u
#define BGA_DISABLED 0x0000u
#define BGA_ENABLED 0x0001u
#define BGA_LFB_ENABLED 0x0040u

#define PCI_CONFIG_ADDRESS_PORT 0x0CF8u
#define PCI_CONFIG_DATA_PORT 0x0CFCu
#define BGA_PCI_VENDOR_ID 0x1234u
#define BGA_PCI_DEVICE_ID 0x1111u

#define VGA_DAC_READ_INDEX 0x3C7u
#define VGA_PEL_MASK 0x3C6u
#define VGA_DAC_WRITE_INDEX 0x3C8u
#define VGA_DAC_DATA 0x3C9u

int vga_init(struct video_mode *mode);

static void bga_write(uint16_t index, uint16_t value) {
    outw(BGA_INDEX_PORT, index);
    outw(BGA_DATA_PORT, value);
}

static uint16_t bga_read(uint16_t index) {
    outw(BGA_INDEX_PORT, index);
    return inw(BGA_DATA_PORT);
}

static uint32_t pci_config_read_u32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t address = 0x80000000u
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)slot << 11)
                     | ((uint32_t)function << 8)
                     | (offset & 0xFCu);

    outl(PCI_CONFIG_ADDRESS_PORT, address);
    return inl(PCI_CONFIG_DATA_PORT);
}

static uint32_t bga_framebuffer_bar0(void) {
    for (uint32_t bus = 0u; bus < 256u; ++bus) {
        for (uint32_t slot = 0u; slot < 32u; ++slot) {
            uint32_t vendor_device = pci_config_read_u32((uint8_t)bus, (uint8_t)slot, 0u, 0x00u);

            if (vendor_device == 0xFFFFFFFFu) {
                continue;
            }
            if ((vendor_device & 0xFFFFu) != BGA_PCI_VENDOR_ID ||
                ((vendor_device >> 16) & 0xFFFFu) != BGA_PCI_DEVICE_ID) {
                continue;
            }

            return pci_config_read_u32((uint8_t)bus, (uint8_t)slot, 0u, 0x10u) & ~0x0Fu;
        }
    }

    return 0u;
}

static int bga_available(void) {
    const uint16_t id = bga_read(BGA_INDEX_ID);
    return id >= BGA_ID0 && id <= BGA_ID5;
}

static int bga_enter_mode(uint16_t width, uint16_t height, uint16_t bpp) {
    if (!bga_available()) {
        return 0;
    }

    bga_write(BGA_INDEX_ENABLE, BGA_DISABLED);
    bga_write(BGA_INDEX_XRES, width);
    bga_write(BGA_INDEX_YRES, height);
    bga_write(BGA_INDEX_BPP, bpp);
    bga_write(BGA_INDEX_BANK, 0u);
    bga_write(BGA_INDEX_ENABLE, BGA_ENABLED | BGA_LFB_ENABLED);
    return 1;
}

static int kernel_video_mode_supported(uint16_t width, uint16_t height) {
    return (width == 640u && height == 480u) ||
           (width == 800u && height == 600u) ||
           (width == 1024u && height == 768u) ||
           (width == 1360u && height == 720u) ||
           (width == 1920u && height == 1080u);
}

static uint32_t kernel_video_mode_bit(uint16_t width, uint16_t height) {
    if (width == 640u && height == 480u) {
        return VIDEO_RES_640X480;
    }
    if (width == 800u && height == 600u) {
        return VIDEO_RES_800X600;
    }
    if (width == 1024u && height == 768u) {
        return VIDEO_RES_1024X768;
    }
    if (width == 1360u && height == 720u) {
        return VIDEO_RES_1360X720;
    }
    if (width == 1920u && height == 1080u) {
        return VIDEO_RES_1920X1080;
    }
    return 0u;
}

static void kernel_video_program_palette(void) {
    if (!g_palette_ready || !g_graphics_enabled || g_fb_bpp != 8u) {
        return;
    }

    outb(VGA_PEL_MASK, 0xFFu);
    outb(VGA_DAC_WRITE_INDEX, 0u);
    for (int i = 0; i < 256 * 3; ++i) {
        outb(VGA_DAC_DATA, (uint8_t)(g_palette[i] >> 2));
    }
}

static void kernel_video_load_default_palette(void) {
    static const uint8_t ega16[16][3] = {
        {0x00u, 0x00u, 0x00u},
        {0x00u, 0x00u, 0xAAu},
        {0x00u, 0xAAu, 0x00u},
        {0x00u, 0xAAu, 0xAAu},
        {0xAAu, 0x00u, 0x00u},
        {0xAAu, 0x00u, 0xAAu},
        {0xAAu, 0x55u, 0x00u},
        {0xAAu, 0xAAu, 0xAAu},
        {0x55u, 0x55u, 0x55u},
        {0x55u, 0x55u, 0xFFu},
        {0x55u, 0xFFu, 0x55u},
        {0x55u, 0xFFu, 0xFFu},
        {0xFFu, 0x55u, 0x55u},
        {0xFFu, 0x55u, 0xFFu},
        {0xFFu, 0xFFu, 0x55u},
        {0xFFu, 0xFFu, 0xFFu}
    };

    for (int i = 0; i < 16; ++i) {
        g_palette[i * 3 + 0] = ega16[i][0];
        g_palette[i * 3 + 1] = ega16[i][1];
        g_palette[i * 3 + 2] = ega16[i][2];
    }
    for (int i = 16; i < 256; ++i) {
        g_palette[i * 3 + 0] = (uint8_t)((((unsigned)i >> 5) & 0x07u) * 255u / 7u);
        g_palette[i * 3 + 1] = (uint8_t)((((unsigned)i >> 2) & 0x07u) * 255u / 7u);
        g_palette[i * 3 + 2] = (uint8_t)(((unsigned)i & 0x03u) * 255u / 3u);
    }
    g_palette_ready = 1;
    kernel_video_program_palette();
}

static uint32_t kernel_video_supported_mode_mask(void) {
    if (g_graphics_bga) {
        return VIDEO_RES_640X480 |
               VIDEO_RES_800X600 |
               VIDEO_RES_1024X768 |
               VIDEO_RES_1360X720 |
               VIDEO_RES_1920X1080;
    }
    if (g_graphics_boot_lfb) {
        return kernel_video_mode_bit((uint16_t)g_mode.width, (uint16_t)g_mode.height);
    }
    return 0u;
}

static void kernel_video_append_cap_mode(struct video_capabilities *caps,
                                         uint16_t width,
                                         uint16_t height) {
    uint32_t index;

    if (caps == NULL || caps->mode_count >= VIDEO_MODE_LIST_MAX) {
        return;
    }

    index = caps->mode_count++;
    caps->mode_width[index] = width;
    caps->mode_height[index] = height;
}

static void kernel_video_fill_capability_modes(struct video_capabilities *caps) {
    if (caps == NULL) {
        return;
    }

    caps->mode_count = 0u;
    for (uint32_t i = 0; i < VIDEO_MODE_LIST_MAX; ++i) {
        caps->mode_width[i] = 0u;
        caps->mode_height[i] = 0u;
    }

    if (g_graphics_bga || (!g_graphics_boot_lfb && bga_available())) {
        kernel_video_append_cap_mode(caps, 640u, 480u);
        kernel_video_append_cap_mode(caps, 800u, 600u);
        kernel_video_append_cap_mode(caps, 1024u, 768u);
        kernel_video_append_cap_mode(caps, 1360u, 720u);
        kernel_video_append_cap_mode(caps, 1920u, 1080u);
        return;
    }

    if (g_graphics_boot_lfb) {
        kernel_video_append_cap_mode(caps, (uint16_t)g_mode.width, (uint16_t)g_mode.height);
    }
}

static size_t kernel_video_mode_buffer_size(const struct video_mode *mode) {
    if (mode == NULL || mode->pitch == 0u || mode->height == 0u) {
        return 0u;
    }
    return (size_t)mode->pitch * (size_t)mode->height;
}

static int kernel_video_mode_usable(const struct video_mode *mode) {
    size_t required_size;
    uint32_t fb_end;

    if (mode == NULL) {
        return 0;
    }
    if (mode->fb_addr < GRAPHICS_MIN_FB_ADDR ||
        mode->width == 0u ||
        mode->height == 0u ||
        mode->width > GRAPHICS_MAX_WIDTH ||
        mode->height > GRAPHICS_MAX_HEIGHT ||
        mode->pitch < mode->width ||
        mode->pitch > GRAPHICS_MAX_PITCH ||
        mode->bpp != GRAPHICS_BPP) {
        return 0;
    }

    required_size = kernel_video_mode_buffer_size(mode);
    if (required_size == 0u || required_size > GRAPHICS_MAX_BYTES) {
        return 0;
    }

    fb_end = mode->fb_addr + (uint32_t)required_size;
    if (fb_end < mode->fb_addr) {
        return 0;
    }
    return 1;
}

static int kernel_video_framebuffer_usable(const struct video_mode *mode) {
    size_t required_size;
    uint32_t fb_end;
    uint32_t min_pitch;

    if (mode == NULL) {
        return 0;
    }
    if (mode->fb_addr < GRAPHICS_MIN_FB_ADDR ||
        mode->width == 0u ||
        mode->height == 0u ||
        mode->width > GRAPHICS_MAX_WIDTH ||
        mode->height > GRAPHICS_MAX_HEIGHT) {
        return 0;
    }

    if (mode->bpp == 8u) {
        min_pitch = mode->width;
    } else if (mode->bpp == 24u) {
        min_pitch = (uint32_t)mode->width * 3u;
    } else if (mode->bpp == 32u) {
        min_pitch = (uint32_t)mode->width * 4u;
    } else {
        return 0;
    }

    if (mode->pitch < min_pitch || mode->pitch > GRAPHICS_MAX_PITCH * 4u) {
        return 0;
    }

    required_size = (size_t)mode->pitch * (size_t)mode->height;
    if (required_size == 0u || required_size > GRAPHICS_MAX_BYTES * 4u) {
        return 0;
    }

    fb_end = mode->fb_addr + (uint32_t)required_size;
    if (fb_end < mode->fb_addr) {
        return 0;
    }
    return 1;
}

static int kernel_video_reserve_backbuffer(size_t required_size) {
    if (required_size == 0u || required_size > GRAPHICS_MAX_BYTES) {
        return 0;
    }
    if (g_graphics_backbuf_capacity >= required_size && g_graphics_backbuf != NULL) {
        return 1;
    }

    g_graphics_backbuf = (uint8_t *)kernel_malloc(required_size);
    if (g_graphics_backbuf == NULL) {
        return 0;
    }

    g_graphics_backbuf_capacity = required_size;
    return 1;
}

static int kernel_video_try_boot_lfb(struct video_mode *mode, uint16_t width, uint16_t height) {
    struct video_mode boot_mode;

    if (mode == NULL) {
        return 0;
    }
    if (vesa_init(&boot_mode) != 0) {
        return 0;
    }
    if (!kernel_video_framebuffer_usable(&boot_mode) ||
        boot_mode.width != width ||
        boot_mode.height != height) {
        return 0;
    }

    mode->fb_addr = boot_mode.fb_addr;
    mode->width = boot_mode.width;
    mode->height = boot_mode.height;
    mode->pitch = boot_mode.width;
    mode->bpp = GRAPHICS_BPP;
    return 1;
}

static int kernel_video_try_init_boot_lfb(void) {
    struct video_mode boot_mode;
    struct video_mode logical_mode;
    size_t required_size;

    if (vesa_init(&boot_mode) != 0) {
        return 0;
    }
    if (!kernel_video_framebuffer_usable(&boot_mode)) {
        return 0;
    }

    logical_mode.fb_addr = boot_mode.fb_addr;
    logical_mode.width = boot_mode.width;
    logical_mode.height = boot_mode.height;
    logical_mode.pitch = boot_mode.width;
    logical_mode.bpp = GRAPHICS_BPP;
    if (!kernel_video_mode_usable(&logical_mode)) {
        return 0;
    }

    required_size = kernel_video_mode_buffer_size(&logical_mode);
    if (required_size == 0u || required_size > sizeof(g_early_graphics_backbuf)) {
        return 0;
    }

    g_graphics_backbuf = g_early_graphics_backbuf;
    g_graphics_backbuf_capacity = sizeof(g_early_graphics_backbuf);
    g_mode = logical_mode;
    g_fb = (volatile uint8_t *)(uintptr_t)g_mode.fb_addr;
    g_fb_pitch = boot_mode.pitch;
    g_fb_bpp = boot_mode.bpp;
    g_backbuf = g_graphics_backbuf;
    g_buf_size = required_size;
    g_graphics_enabled = 1;
    g_graphics_bga = 0;
    g_graphics_boot_lfb = 1;

    kernel_video_load_default_palette();
    for (size_t i = 0; i < g_buf_size; ++i) {
        g_backbuf[i] = 0u;
    }
    kernel_video_flip();
    kernel_debug_printf("video: boot LFB active fb=%x size=%d\n",
                        (unsigned int)g_mode.fb_addr,
                        (int)g_buf_size);
    return 1;
}

static int kernel_video_try_init_early_bga(void) {
    size_t required_size;
    uint32_t fb_addr;

    if (!bga_enter_mode((uint16_t)GRAPHICS_DEFAULT_WIDTH,
                        (uint16_t)GRAPHICS_DEFAULT_HEIGHT,
                        (uint16_t)GRAPHICS_BPP)) {
        return 0;
    }
    fb_addr = bga_framebuffer_bar0();
    if (fb_addr < GRAPHICS_MIN_FB_ADDR) {
        return 0;
    }

    g_mode.width = GRAPHICS_DEFAULT_WIDTH;
    g_mode.height = GRAPHICS_DEFAULT_HEIGHT;
    g_mode.pitch = GRAPHICS_DEFAULT_WIDTH;
    g_mode.bpp = GRAPHICS_BPP;
    g_mode.fb_addr = fb_addr;
    if (!kernel_video_mode_usable(&g_mode)) {
        return 0;
    }

    required_size = kernel_video_mode_buffer_size(&g_mode);
    if (required_size == 0u || required_size > sizeof(g_early_graphics_backbuf)) {
        return 0;
    }

    g_graphics_backbuf = g_early_graphics_backbuf;
    g_graphics_backbuf_capacity = sizeof(g_early_graphics_backbuf);
    g_fb = (volatile uint8_t *)(uintptr_t)g_mode.fb_addr;
    g_fb_pitch = g_mode.pitch;
    g_fb_bpp = GRAPHICS_BPP;
    g_backbuf = g_graphics_backbuf;
    g_buf_size = required_size;
    g_graphics_enabled = 1;
    g_graphics_bga = 1;
    g_graphics_boot_lfb = 0;

    kernel_video_load_default_palette();
    for (size_t i = 0; i < g_buf_size; ++i) {
        g_backbuf[i] = 0u;
    }
    kernel_video_flip();
    kernel_debug_printf("video: early BGA active fb=%x size=%d\n",
                        (unsigned int)g_mode.fb_addr,
                        (int)g_buf_size);
    return 1;
}

static int kernel_video_apply_graphics_mode(uint16_t width, uint16_t height) {
    int use_bga = 0;
    struct video_mode selected_mode;
    struct video_mode boot_mode;
    size_t required_size;

    if (!kernel_video_mode_supported(width, height)) {
        return -1;
    }

    kernel_debug_printf("video: mode request %dx%d\n", (int)width, (int)height);
    kernel_keyboard_prepare_for_graphics();
    kernel_mouse_prepare_for_graphics();

    if (bga_available()) {
        use_bga = bga_enter_mode(width, height, (uint16_t)GRAPHICS_BPP);
        if (!use_bga) {
            kernel_debug_puts("video: BGA mode switch failed\n");
            return -1;
        }
        selected_mode.width = width;
        selected_mode.height = height;
        selected_mode.pitch = width;
        selected_mode.bpp = GRAPHICS_BPP;
        selected_mode.fb_addr = bga_framebuffer_bar0();
        if (selected_mode.fb_addr < GRAPHICS_MIN_FB_ADDR) {
            kernel_debug_puts("video: BGA framebuffer BAR missing\n");
            return -1;
        }
        kernel_debug_puts("video: BGA mode switch complete\n");
    } else if (kernel_video_try_boot_lfb(&selected_mode, width, height)) {
        kernel_debug_printf("video: using boot LFB fb=%x pitch=%d\n",
                            (unsigned int)selected_mode.fb_addr,
                            (int)selected_mode.pitch);
    } else {
        kernel_debug_puts("video: no compatible BGA or boot LFB mode\n");
        return -1;
    }

    required_size = kernel_video_mode_buffer_size(&selected_mode);
    if (!kernel_video_reserve_backbuffer(required_size)) {
        kernel_debug_printf("video: backbuffer alloc failed for %d bytes\n", (int)required_size);
        return -1;
    }

    g_graphics_bga = use_bga;
    g_graphics_boot_lfb = !use_bga;
    g_mode = selected_mode;
    g_fb = (volatile uint8_t *)(uintptr_t)g_mode.fb_addr;
    if (use_bga) {
        g_fb_pitch = g_mode.pitch;
        g_fb_bpp = GRAPHICS_BPP;
    } else if (vesa_init(&boot_mode) == 0 &&
               boot_mode.width == g_mode.width &&
               boot_mode.height == g_mode.height) {
        g_fb_pitch = boot_mode.pitch;
        g_fb_bpp = boot_mode.bpp;
    } else {
        g_fb_pitch = g_mode.pitch;
        g_fb_bpp = GRAPHICS_BPP;
    }
    g_backbuf = g_graphics_backbuf;
    g_buf_size = required_size;
    g_graphics_enabled = 1;

    kernel_video_load_default_palette();
    for (size_t i = 0; i < g_buf_size; ++i) {
        g_backbuf[i] = 0u;
    }

    kernel_mouse_sync_to_video();
    kernel_video_flip();
    kernel_debug_printf("video: graphics active fb=%x size=%d\n",
                        (unsigned int)g_mode.fb_addr,
                        (int)g_buf_size);
    return 0;
}

static void kernel_video_enter_graphics(void) {
    if (g_graphics_enabled) {
        return;
    }
    (void)kernel_video_apply_graphics_mode((uint16_t)GRAPHICS_DEFAULT_WIDTH,
                                           (uint16_t)GRAPHICS_DEFAULT_HEIGHT);
}

void kernel_video_init(void) {
    if (g_video_initialized) {
        return;
    }

    if (kernel_video_try_init_boot_lfb()) {
        g_video_initialized = 1;
        return;
    }
    if (kernel_video_try_init_early_bga()) {
        g_video_initialized = 1;
        return;
    }

    /* stay in BIOS text mode until a graphical syscall is used */
    g_mode.fb_addr = 0xB8000;
    g_mode.width = 80;
    g_mode.height = 25;
    g_mode.pitch = 160; /* 80 cols * 2 bytes */
    g_mode.bpp = 16;
    g_fb = (volatile uint8_t *)g_mode.fb_addr;
    g_fb_pitch = 160u;
    g_fb_bpp = 16u;
    g_backbuf = NULL;
    g_buf_size = 0;
    g_video_initialized = 1;
}

struct video_mode *kernel_video_get_mode(void) {
    return &g_mode;
}

volatile uint8_t *kernel_video_get_fb(void) {
    return g_fb;
}

uint8_t *kernel_video_get_backbuffer(void) {
    return g_backbuf;
}

size_t kernel_video_get_pixel_count(void) {
    return g_buf_size;
}

void kernel_video_clear(uint8_t color) {
    if (!g_graphics_enabled || g_backbuf == NULL) {
        (void)color;
        return;
    }

    for (size_t i = 0; i < g_buf_size; ++i) {
        g_backbuf[i] = color;
    }
}

void kernel_video_flip(void) {
    if (!g_graphics_enabled || g_backbuf == NULL || g_fb == NULL) {
        return;
    }

    if (g_graphics_bga && g_mode.fb_addr == 0xA0000u && g_buf_size > GRAPHICS_BANK_SIZE) {
        size_t offset = 0u;
        uint16_t bank = 0u;

        while (offset < g_buf_size) {
            size_t count = g_buf_size - offset;
            if (count > GRAPHICS_BANK_SIZE) {
                count = GRAPHICS_BANK_SIZE;
            }

            bga_write(BGA_INDEX_BANK, bank);
            for (size_t i = 0; i < count; ++i) {
                g_fb[i] = g_backbuf[offset + i];
            }

            offset += count;
            bank = (uint16_t)(bank + 1u);
        }

        bga_write(BGA_INDEX_BANK, 0u);
        return;
    }

    if (g_fb_bpp == 8u) {
        if (g_fb_pitch == g_mode.pitch) {
            for (size_t i = 0; i < g_buf_size; ++i) {
                g_fb[i] = g_backbuf[i];
            }
        } else {
            for (uint32_t y = 0u; y < g_mode.height; ++y) {
                uint32_t src_row = y * g_mode.pitch;
                uint32_t dst_row = y * g_fb_pitch;

                for (uint32_t x = 0u; x < g_mode.width; ++x) {
                    g_fb[dst_row + x] = g_backbuf[src_row + x];
                }
            }
        }
        return;
    }

    if ((g_fb_bpp == 24u || g_fb_bpp == 32u) && g_palette_ready) {
        uint32_t bytes_per_pixel = (uint32_t)(g_fb_bpp / 8u);

        for (uint32_t y = 0u; y < g_mode.height; ++y) {
            uint32_t src_row = y * g_mode.pitch;
            uint32_t dst_row = y * g_fb_pitch;

            for (uint32_t x = 0u; x < g_mode.width; ++x) {
                uint8_t index = g_backbuf[src_row + x];
                uint32_t dst = dst_row + (x * bytes_per_pixel);
                g_fb[dst + 0u] = g_palette[index * 3u + 2u];
                g_fb[dst + 1u] = g_palette[index * 3u + 1u];
                g_fb[dst + 2u] = g_palette[index * 3u + 0u];
                if (g_fb_bpp == 32u) {
                    g_fb[dst + 3u] = 0u;
                }
            }
        }
    }
}

void kernel_video_leave_graphics(void) {
    if (g_graphics_boot_lfb) {
        kernel_video_clear(0u);
        kernel_video_flip();
        kernel_text_init();
        return;
    }

    if (g_graphics_bga) {
        bga_write(BGA_INDEX_ENABLE, BGA_DISABLED);
        bga_write(BGA_INDEX_BANK, 0u);
    }

    g_graphics_enabled = 0;
    g_graphics_bga = 0;
    g_graphics_boot_lfb = 0;
    g_mode.fb_addr = 0xB8000u;
    g_mode.width = 80u;
    g_mode.height = 25u;
    g_mode.pitch = 160u;
    g_mode.bpp = 16u;
    g_fb = (volatile uint8_t *)(uintptr_t)g_mode.fb_addr;
    g_fb_pitch = 160u;
    g_fb_bpp = 16u;
    g_backbuf = NULL;
    g_buf_size = 0u;
    kernel_text_init();
}

int kernel_video_set_mode(uint32_t width, uint32_t height) {
    if (g_graphics_boot_lfb &&
        (width != g_mode.width || height != g_mode.height)) {
        return -1;
    }
    return kernel_video_apply_graphics_mode((uint16_t)width, (uint16_t)height);
}

void kernel_video_get_capabilities(struct video_capabilities *caps) {
    if (caps == NULL) {
        return;
    }

    caps->flags = 0u;
    caps->supported_modes = 0u;
    caps->active_width = g_mode.width;
    caps->active_height = g_mode.height;
    caps->active_bpp = g_mode.bpp;
    kernel_video_fill_capability_modes(caps);

    if (g_graphics_bga) {
        caps->flags |= VIDEO_CAPS_BGA | VIDEO_CAPS_CAN_SET_MODE;
        caps->supported_modes = kernel_video_supported_mode_mask();
    } else if (g_graphics_boot_lfb) {
        caps->flags |= VIDEO_CAPS_BOOT_LFB;
        caps->supported_modes = kernel_video_supported_mode_mask();
    } else if (bga_available()) {
        caps->flags |= VIDEO_CAPS_BGA | VIDEO_CAPS_CAN_SET_MODE;
        caps->supported_modes = VIDEO_RES_640X480 |
                                VIDEO_RES_800X600 |
                                VIDEO_RES_1024X768 |
                                VIDEO_RES_1360X720 |
                                VIDEO_RES_1920X1080;
        kernel_video_fill_capability_modes(caps);
    } else {
        caps->flags |= VIDEO_CAPS_TEXT_ONLY;
    }
}

/* graphics helper internal font & routines copied from stage2 */

static char uppercase_char(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    return c;
}

static uint8_t font_row_bits(char c, int row) {
    c = uppercase_char(c);
    /* the big switch from stage2/graphics.c */
    switch (c) {
        case 'A': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; return g[row]; }
        case 'B': { static const uint8_t g[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; return g[row]; }
        case 'C': { static const uint8_t g[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; return g[row]; }
        case 'D': { static const uint8_t g[7] = {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}; return g[row]; }
        case 'E': { static const uint8_t g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; return g[row]; }
        case 'F': { static const uint8_t g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; return g[row]; }
        case 'G': { static const uint8_t g[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}; return g[row]; }
        case 'H': { static const uint8_t g[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; return g[row]; }
        case 'I': { static const uint8_t g[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}; return g[row]; }
        case 'J': { static const uint8_t g[7] = {0x1F,0x02,0x02,0x02,0x12,0x12,0x0C}; return g[row]; }
        case 'K': { static const uint8_t g[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11}; return g[row]; }
        case 'L': { static const uint8_t g[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; return g[row]; }
        case 'M': { static const uint8_t g[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; return g[row]; }
        case 'N': { static const uint8_t g[7] = {0x11,0x11,0x19,0x15,0x13,0x11,0x11}; return g[row]; }
        case 'O': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; return g[row]; }
        case 'P': { static const uint8_t g[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; return g[row]; }
        case 'Q': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; return g[row]; }
        case 'R': { static const uint8_t g[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; return g[row]; }
        case 'S': { static const uint8_t g[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; return g[row]; }
        case 'T': { static const uint8_t g[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; return g[row]; }
        case 'U': { static const uint8_t g[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; return g[row]; }
        case 'V': { static const uint8_t g[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; return g[row]; }
        case 'W': { static const uint8_t g[7] = {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}; return g[row]; }
        case 'X': { static const uint8_t g[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; return g[row]; }
        case 'Y': { static const uint8_t g[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; return g[row]; }
        case 'Z': { static const uint8_t g[7] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}; return g[row]; }
        case '0': { static const uint8_t g[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; return g[row]; }
        case '1': { static const uint8_t g[7] = {0x04,0x0C,0x14,0x04,0x04,0x04,0x1F}; return g[row]; }
        case '2': { static const uint8_t g[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; return g[row]; }
        case '3': { static const uint8_t g[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}; return g[row]; }
        case '4': { static const uint8_t g[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; return g[row]; }
        case '5': { static const uint8_t g[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}; return g[row]; }
        case '6': { static const uint8_t g[7] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}; return g[row]; }
        case '7': { static const uint8_t g[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; return g[row]; }
        case '8': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; return g[row]; }
        case '9': { static const uint8_t g[7] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}; return g[row]; }
        case '>': { static const uint8_t g[7] = {0x10,0x08,0x04,0x02,0x04,0x08,0x10}; return g[row]; }
        case '<': { static const uint8_t g[7] = {0x01,0x02,0x04,0x08,0x04,0x02,0x01}; return g[row]; }
        case ':': { static const uint8_t g[7] = {0x00,0x04,0x04,0x00,0x04,0x04,0x00}; return g[row]; }
        case '-': { static const uint8_t g[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}; return g[row]; }
        case '_': { static const uint8_t g[7] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}; return g[row]; }
        case '.': { static const uint8_t g[7] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}; return g[row]; }
        case '/': { static const uint8_t g[7] = {0x01,0x02,0x04,0x08,0x10,0x00,0x00}; return g[row]; }
        case '\\': { static const uint8_t g[7] = {0x10,0x08,0x04,0x02,0x01,0x00,0x00}; return g[row]; }
        case '[': { static const uint8_t g[7] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}; return g[row]; }
        case ']': { static const uint8_t g[7] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}; return g[row]; }
        case '=': { static const uint8_t g[7] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}; return g[row]; }
        case '+': { static const uint8_t g[7] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}; return g[row]; }
        case '(':{ static const uint8_t g[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02}; return g[row]; }
        case ')':{ static const uint8_t g[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08}; return g[row]; }
        case '?':{ static const uint8_t g[7] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}; return g[row]; }
        case '!':{ static const uint8_t g[7] = {0x04,0x04,0x04,0x04,0x04,0x00,0x04}; return g[row]; }
        case ' ': return 0x00;
        default: { static const uint8_t g[7] = {0x1F,0x01,0x05,0x09,0x11,0x00,0x11}; return g[row]; }
    }
}

void kernel_gfx_putpixel(int x, int y, uint8_t color) {
    kernel_video_enter_graphics();
    struct video_mode *mode = kernel_video_get_mode();
    uint8_t *bb = kernel_video_get_backbuffer();

    if (!bb) return;
    if (x < 0 || y < 0 || x >= (int)mode->width || y >= (int)mode->height) return;
    bb[(y * mode->pitch) + x] = color;
}

void kernel_gfx_rect(int x, int y, int w, int h, uint8_t color) {
    kernel_video_enter_graphics();
    struct video_mode *mode = kernel_video_get_mode();
    uint8_t *bb = kernel_video_get_backbuffer();
    if (!bb || w <= 0 || h <= 0) return;
    int x0 = x<0?0:x;
    int y0 = y<0?0:y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > (int)mode->width) x1 = (int)mode->width;
    if (y1 > (int)mode->height) y1 = (int)mode->height;
    for (int py=y0; py<y1; ++py)
        for (int px=x0; px<x1; ++px)
            bb[(py * mode->pitch) + px] = color;
}

void kernel_gfx_clear(uint8_t color) {
    kernel_video_enter_graphics();
    kernel_video_clear(color);
}

void kernel_gfx_draw_text(int x, int y, const char *text, uint8_t color) {
    kernel_video_enter_graphics();
    int cx = x, cy = y;
    while (*text) {
        char c = *text++;
        if (c=='\n') { cx = x; cy += 8; continue; }
        /* draw char */
        for (int row=0; row<7; ++row) {
            uint8_t bits = font_row_bits(c,row);
            for (int col=0; col<5; ++col) {
                if (!(bits & (1u << (4-col)))) continue;
                int px = cx + col;
                int py = cy + row;
                if (px<0||py<0||px>= (int)g_mode.width||py>=(int)g_mode.height) continue;
                g_backbuf[(py * g_mode.pitch) + px] = color;
            }
        }
        cx += 6;
    }
}

void kernel_gfx_blit8(const uint8_t *src, int src_w, int src_h, int dst_x, int dst_y, int scale) {
    struct video_mode *mode;
    uint8_t *bb;

    kernel_video_enter_graphics();
    if (src == NULL || src_w <= 0 || src_h <= 0 || scale <= 0) {
        return;
    }

    mode = kernel_video_get_mode();
    bb = kernel_video_get_backbuffer();
    if (mode == NULL || bb == NULL) {
        return;
    }

    for (int sy = 0; sy < src_h; ++sy) {
        int py0 = dst_y + (sy * scale);
        for (int sx = 0; sx < src_w; ++sx) {
            int px0 = dst_x + (sx * scale);
            uint8_t color = src[(sy * src_w) + sx];

            for (int oy = 0; oy < scale; ++oy) {
                int py = py0 + oy;
                if (py < 0 || py >= (int)mode->height) {
                    continue;
                }
                for (int ox = 0; ox < scale; ++ox) {
                    int px = px0 + ox;
                    if (px < 0 || px >= (int)mode->width) {
                        continue;
                    }
                    bb[(py * mode->pitch) + px] = color;
                }
            }
        }
    }
}

int kernel_video_set_palette(const uint8_t *rgb_triplets) {
    if (rgb_triplets == NULL) {
        return -1;
    }

    for (int i = 0; i < 256 * 3; ++i) {
        g_palette[i] = rgb_triplets[i];
    }
    g_palette_ready = 1;
    kernel_video_program_palette();
    return 0;
}

int kernel_video_get_palette(uint8_t *rgb_triplets) {
    if (rgb_triplets == NULL) {
        return -1;
    }

    if (!g_palette_ready) {
        kernel_video_load_default_palette();
    }
    for (int i = 0; i < 256 * 3; ++i) {
        rgb_triplets[i] = g_palette[i];
    }
    return 0;
}
