#include <kernel/bootinfo.h>
#include <string.h>
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

static int host_try_external_argv(int argc, char **argv) {
    lang_invalidate_directory_cache();
    return lang_try_run(argc, argv);
}

static int desktop_host_pid_alive(int pid) {
    struct task_snapshot_summary summary;
    struct task_snapshot_entry entries[TASK_SNAPSHOT_MAX];
    uint32_t count;
    uint32_t i;

    if (pid <= 0) {
        return 0;
    }

    memset(&summary, 0, sizeof(summary));
    memset(entries, 0, sizeof(entries));
    count = (uint32_t)sys_task_snapshot(&summary, entries, TASK_SNAPSHOT_MAX);
    for (i = 0; i < count; ++i) {
        if ((int)entries[i].pid == pid) {
            return 1;
        }
    }
    return 0;
}

static int desktop_host_launch_startx_session(void) {
    int pid = sys_launch_builtin_user(USERLAND_BUILTIN_STARTX);

    if (pid > 0) {
        host_debug("host: desktop session launched", 0);
        return pid;
    }

    host_debug("host: desktop session launch failed", 0);
    return -1;
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
    int session_pid = -1;

    host_debug("host: desktop start", 0);
    console_init();
    fs_init();

    if (sys_launch_info(&info) == 0 &&
        (info.boot_flags & (BOOTINFO_FLAG_BOOT_SAFE_MODE | BOOTINFO_FLAG_BOOT_RESCUE_SHELL)) == 0u) {
        session_pid = desktop_host_launch_startx_session();
        if (session_pid > 0) {
            for (;;) {
                if (!desktop_host_pid_alive(session_pid)) {
                    host_debug("host: desktop session exited", 0);
                    session_pid = desktop_host_launch_startx_session();
                    if (session_pid <= 0) {
                        break;
                    }
                }
                sys_yield();
            }
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

void userland_startx_host_entry(void) {
    host_debug("host: startx start", 0);
    console_init();
    fs_init();

    if (desktop_host_try_external() == 0) {
        host_debug("host: startx external returned", 0);
        return;
    }

    host_debug("host: startx fallback builtin", 0);
    desktop_main();
    host_debug("host: startx builtin returned", 0);
}

void userland_desktop_audio_host_entry(void) {
    char *argv[4] = {"audiosvc", "play-asset", "/assets/vibe_os_desktop.wav", 0};

    host_debug("host: desktop audio start", 0);
    console_init();
    fs_init();

    if (host_try_external_argv(3, argv) == 0) {
        host_debug("host: desktop audio returned", 0);
        return;
    }

    host_debug("host: desktop audio fallback", 0);
    (void)audio_play_wav_best_effort("/assets/vibe_os_desktop.wav", "desktop-session");
    host_debug("host: desktop audio done", 0);
}
