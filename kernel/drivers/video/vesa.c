#include <kernel/bootinfo.h>
#include <kernel/drivers/video/video.h>

int vesa_init(struct video_mode *mode) {
    const volatile struct bootinfo *bootinfo =
        (const volatile struct bootinfo *)(uintptr_t)BOOTINFO_ADDR;

    if (bootinfo->magic != BOOTINFO_MAGIC ||
        bootinfo->version != BOOTINFO_VERSION ||
        (bootinfo->flags & BOOTINFO_FLAG_VESA_VALID) == 0u ||
        bootinfo->vesa.mode == 0u) {
        return -1;
    }

    mode->fb_addr = bootinfo->vesa.fb_addr;
    mode->pitch = bootinfo->vesa.pitch;
    mode->width = bootinfo->vesa.width;
    mode->height = bootinfo->vesa.height;
    mode->bpp = bootinfo->vesa.bpp;
    if (mode->fb_addr == 0u || mode->width == 0u || mode->height == 0u || mode->pitch < mode->width) {
        return -1;
    }
    return 0;
}
