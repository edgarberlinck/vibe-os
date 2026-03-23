#include <userland/modules/include/console.h>
#include <userland/modules/include/fs.h>
#include <userland/modules/include/lang_loader.h>
#include <userland/modules/include/syscalls.h>
#include <userland/modules/include/utils.h>

#define BOOTSTRAP_LINE_MAX 128
#define BOOTSTRAP_ARGV_MAX 16

static void bootstrap_print_banner(void) {
    console_write("VibeOS bootstrap init\n");
    console_write("kernel pequeno, apps externas via AppFS\n");
    console_write("comandos: help, apps, clear, shutdown, run <app> [args]\n");
}

static void bootstrap_print_apps(void) {
    console_write("apps: hello js ruby python java javac echo cat wc pwd head tail grep loadkeys sleep rmdir true false printf\n");
}

static int bootstrap_is_printable(int ch) {
    return ch >= 32 && ch <= 126;
}

static void bootstrap_read_line(char *buffer, int capacity) {
    int length = 0;

    if (!buffer || capacity <= 0) {
        return;
    }

    for (;;) {
        int ch = console_getchar();

        if (ch == '\r' || ch == '\n') {
            console_putc('\n');
            break;
        }

        if (ch == 8 || ch == 127) {
            if (length > 0) {
                --length;
                buffer[length] = '\0';
                console_putc('\b');
                console_putc(' ');
                console_putc('\b');
            }
            continue;
        }

        if (!bootstrap_is_printable(ch) || length >= capacity - 1) {
            continue;
        }

        buffer[length++] = (char)ch;
        buffer[length] = '\0';
        console_putc((char)ch);
    }

    buffer[length] = '\0';
}

static char *bootstrap_basename(char *name) {
    char *cursor = name;
    char *base = name;

    while (cursor && *cursor != '\0') {
        if (*cursor == '/') {
            base = cursor + 1;
        }
        ++cursor;
    }
    return base;
}

static int bootstrap_tokenize(char *line, char **argv, int max_args) {
    int argc = 0;
    char *cursor = line;
    char *token;

    while (argc < max_args && (token = next_token(&cursor)) != 0) {
        argv[argc++] = token;
    }
    return argc;
}

static void bootstrap_run_command(char *line) {
    char *argv[BOOTSTRAP_ARGV_MAX];
    int argc;
    int rc;

    argc = bootstrap_tokenize(line, argv, BOOTSTRAP_ARGV_MAX);
    if (argc <= 0) {
        return;
    }

    if (str_eq(argv[0], "help")) {
        bootstrap_print_banner();
        return;
    }

    if (str_eq(argv[0], "apps")) {
        bootstrap_print_apps();
        return;
    }

    if (str_eq(argv[0], "clear")) {
        console_clear();
        return;
    }

    if (str_eq(argv[0], "shutdown")) {
        console_write("desligando...\n");
        sys_shutdown();
        return;
    }

    if (str_eq(argv[0], "run")) {
        if (argc < 2) {
            console_write("uso: run <app> [args]\n");
            return;
        }
        argv[1] = bootstrap_basename(argv[1]);
        rc = lang_try_run(argc - 1, &argv[1]);
    } else {
        argv[0] = bootstrap_basename(argv[0]);
        rc = lang_try_run(argc, argv);
    }

    if (rc != 0) {
        console_write("erro: app nao encontrada no AppFS\n");
    }
}

static void bootstrap_run_startup_smoke(void) {
    char *argv[2];
    int rc;
    extern void kernel_debug_puts(const char *);

    argv[0] = "hello";
    argv[1] = 0;

    kernel_debug_puts("init: startup smoke begin\n");
    rc = lang_try_run(1, argv);
    if (rc == 0) {
        kernel_debug_puts("init: startup smoke returned\n");
    } else {
        kernel_debug_puts("init: startup smoke failed\n");
    }
}

__attribute__((section(".entry"))) void userland_entry(void) {
    static char line[BOOTSTRAP_LINE_MAX];
    extern void kernel_debug_puts(const char *);

    kernel_debug_puts("init: entered builtin entry\n");
    sys_write_debug("init: builtin bootstrap launching external AppFS apps\n");
    kernel_debug_puts("init: sys_write_debug returned\n");
    console_init();
    kernel_debug_puts("init: console_init returned\n");
    fs_init();
    kernel_debug_puts("init: fs_init returned\n");
    kernel_debug_puts("init: second sys_write_debug begin\n");
    sys_write_debug("init: appfs launcher ready\n");
    kernel_debug_puts("init: second sys_write_debug returned\n");
    kernel_debug_puts("init: banner begin\n");
    bootstrap_print_banner();
    kernel_debug_puts("init: banner returned\n");
    bootstrap_run_startup_smoke();

    for (;;) {
        console_write("init> ");
        bootstrap_read_line(line, (int)sizeof(line));
        bootstrap_run_command(line);
        fs_tick();
    }
}
