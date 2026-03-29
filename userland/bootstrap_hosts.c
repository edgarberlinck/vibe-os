#include <kernel/bootinfo.h>
#include <userland/modules/include/busybox.h>
#include <userland/modules/include/console.h>
#include <userland/modules/include/fs.h>
#include <userland/modules/include/lang_loader.h>
#include <userland/modules/include/shell.h>
#include <userland/modules/include/syscalls.h>
#include <userland/modules/include/ui.h>
#include <userland/modules/include/utils.h>

static void host_debug(const char *prefix, const char *suffix) {
    char msg[96];

    msg[0] = '\0';
    str_append(msg, prefix, (int)sizeof(msg));
    if (suffix != 0) {
        str_append(msg, suffix, (int)sizeof(msg));
    }
    str_append(msg, "\n", (int)sizeof(msg));
    sys_write_debug(msg);
}

static int desktop_host_try_external(void) {
    char *argv[2] = {"startx", 0};

    lang_invalidate_directory_cache();
    return lang_try_run(1, argv);
}

void userland_shell_host_entry(void) {
    host_debug("host: shell start", 0);
    console_init();
    fs_init();
    shell_start_ready();
    host_debug("host: shell returned", 0);
    for (;;) {
        sys_yield();
    }
}

void userland_desktop_host_entry(void) {
    struct userland_launch_info info;

    host_debug("host: desktop start", 0);
    console_init();
    fs_init();

    if (sys_launch_info(&info) == 0 &&
        (info.boot_flags & (BOOTINFO_FLAG_BOOT_SAFE_MODE | BOOTINFO_FLAG_BOOT_RESCUE_SHELL)) == 0u) {
        if (desktop_host_try_external() == 0) {
            host_debug("host: desktop external returned", 0);
        } else {
            host_debug("host: desktop fallback builtin", 0);
            desktop_main();
        }
    } else {
        host_debug("host: desktop denied by boot flags", 0);
    }

    host_debug("host: desktop -> shell fallback", 0);
    shell_start_ready();
    for (;;) {
        sys_yield();
    }
}
