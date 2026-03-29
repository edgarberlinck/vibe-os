#include <userland/modules/include/console.h>
#include <userland/modules/include/fs.h>
#include <userland/modules/include/syscalls.h>
#include <userland/modules/include/utils.h>
#include <kernel/bootinfo.h>
#include <kernel/drivers/storage/ata.h>

#define BOOTSTRAP_STORAGE_SMOKE_SECTOR (KERNEL_PERSIST_START_LBA + KERNEL_PERSIST_SECTOR_COUNT - 1u)
#define BOOTSTRAP_STORAGE_SMOKE_SIZE 512u

static void bootstrap_print_banner(void) {
    struct userland_launch_info info;

    sys_write_debug("VibeOS bootstrap init\n");
    sys_write_debug("kernel pequeno, apps externas via AppFS\n");
    sys_write_debug("userland.app carregada automaticamente no boot\n");
    sys_write_debug("use 'help' para listar comandos e apps modulares\n");
    sys_write_debug("atalhos graficos: startx, edit, nano\n");
    if (sys_launch_info(&info) == 0) {
        if ((info.boot_flags & BOOTINFO_FLAG_BOOT_SAFE_MODE) != 0u) {
            sys_write_debug("boot mode: safe mode\n");
        } else if ((info.boot_flags & BOOTINFO_FLAG_BOOT_RESCUE_SHELL) != 0u) {
            sys_write_debug("boot mode: rescue shell\n");
        }
        if ((info.boot_flags & BOOTINFO_FLAG_EXPERIMENTAL_I915_COMMIT) != 0u) {
            sys_write_debug("video: i915 experimental commit enabled\n");
        }
        if ((info.boot_flags & BOOTINFO_FLAG_FORCE_LEGACY_VIDEO) != 0u) {
            sys_write_debug("video: legacy video driver forced\n");
        }
    }
}

static int bootstrap_run_startup_apps(void) {
    struct userland_launch_info info;

    if (sys_launch_info(&info) == 0 &&
        (info.boot_flags & BOOTINFO_FLAG_BOOT_RESCUE_SHELL) != 0u) {
        sys_write_debug("init: rescue shell requested, skipping desktop launch\n");
        return -2;
    }

    if ((info.boot_flags & BOOTINFO_FLAG_BOOT_TO_DESKTOP) != 0u &&
        (info.boot_flags & (BOOTINFO_FLAG_BOOT_SAFE_MODE | BOOTINFO_FLAG_BOOT_RESCUE_SHELL)) == 0u) {
        int pid = sys_launch_builtin_user(USERLAND_BUILTIN_DESKTOP);

        if (pid > 0) {
            sys_write_debug("init: desktop host launched\n");
            return 0;
        }
        sys_write_debug("init: desktop host launch failed\n");
    }

    return -1;
}

static void bootstrap_storage_smoke_test(void) {
    uint8_t verify[BOOTSTRAP_STORAGE_SMOKE_SIZE];
    uint8_t verify_again[BOOTSTRAP_STORAGE_SMOKE_SIZE];
    extern void kernel_debug_puts(const char *);

    kernel_debug_puts("init: storage smoke begin\n");
    if (sys_storage_read_sectors(BOOTSTRAP_STORAGE_SMOKE_SECTOR, verify, 1u) != 0) {
        kernel_debug_puts("init: storage smoke read failed\n");
        return;
    }
    if (sys_storage_read_sectors(BOOTSTRAP_STORAGE_SMOKE_SECTOR, verify_again, 1u) != 0) {
        kernel_debug_puts("init: storage smoke verify read failed\n");
        return;
    }
    for (uint32_t i = 0; i < BOOTSTRAP_STORAGE_SMOKE_SIZE; ++i) {
        if (verify[i] != verify_again[i]) {
            kernel_debug_puts("init: storage smoke verify mismatch\n");
            return;
        }
    }
    kernel_debug_puts("init: storage smoke ok\n");
}

static void bootstrap_try_play_boot_sound(void) {
    (void)audio_play_wav_best_effort("/assets/vibe_os_boot.wav", "boot");
    sys_write_debug("init: boot sound returned\n");
}

__attribute__((section(".entry"))) void userland_entry(void) {
    extern void kernel_debug_puts(const char *);
    int rc;
    struct userland_launch_info info;

    kernel_debug_puts("init: entered builtin entry\n");
    if (sys_launch_info(&info) == 0 &&
        (info.boot_flags & BOOTINFO_FLAG_EXPERIMENTAL_I915_COMMIT) != 0u) {
        kernel_debug_puts("init: boot flag enabled for i915 experimental commit\n");
    }
    if (sys_launch_info(&info) == 0 &&
        (info.boot_flags & BOOTINFO_FLAG_FORCE_LEGACY_VIDEO) != 0u) {
        kernel_debug_puts("init: boot flag forcing legacy video driver\n");
    }
    sys_write_debug("init: builtin bootstrap launching external AppFS apps\n");
    kernel_debug_puts("init: sys_write_debug returned\n");

    console_init();
    kernel_debug_puts("init: console_init returned\n");

    fs_init();
    kernel_debug_puts("init: fs_init returned\n");
    bootstrap_storage_smoke_test();
    if ((info.boot_flags & BOOTINFO_FLAG_BOOT_TO_DESKTOP) == 0u ||
        (info.boot_flags & (BOOTINFO_FLAG_BOOT_SAFE_MODE | BOOTINFO_FLAG_BOOT_RESCUE_SHELL)) != 0u) {
        bootstrap_try_play_boot_sound();
    } else {
        sys_write_debug("init: boot sound deferred for desktop boot\n");
    }

    sys_write_debug("init: appfs launcher ready\n");
    kernel_debug_puts("init: appfs launcher ready\n");

    bootstrap_print_banner();
    kernel_debug_puts("init: banner returned\n");

    rc = bootstrap_run_startup_apps();
    if (rc != 0) {
        int shell_pid = sys_launch_builtin_user(USERLAND_BUILTIN_SHELL);

        if (shell_pid > 0) {
            if (rc == -2) {
                console_write("init: rescue shell host ativa\n");
            } else {
                console_write("init: fallback para shell host embutida\n");
            }
            kernel_debug_puts("init: shell host launched\n");
        } else {
            console_write("init: shell host launch failed\n");
            kernel_debug_puts("init: shell host launch failed\n");
        }
    }
    kernel_debug_puts("init: supervisor idle\n");
    for (;;)
        sys_yield();
}
