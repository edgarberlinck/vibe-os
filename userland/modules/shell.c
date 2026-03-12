#include <userland/modules/include/shell.h>
#include <kernel/drivers/input/input.h>   /* kernel_keyboard_read */
#include <kernel/scheduler.h>              /* yield */
#include <stdint.h>

#define VGA_MEM ((volatile uint16_t *)0xB8000)
#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_ATTR 0x0F
#define LINE_MAX 128

/* history stubs for busybox compatibility */
void shell_history_add(const char *line) { (void)line; }
void shell_history_print(void) { /* no-op */ }

static int cur_x = 0, cur_y = 0;
static volatile uint32_t shell_state = 0;
static volatile uint32_t shell_last_len = 0;
static volatile uint32_t shell_last_cmd0 = 0;

static void vga_putc(char c) {
    if (c == 0) return;
    if (c == '\n') {
        cur_x = 0;
        cur_y++;
    } else if (c == '\r') {
        cur_x = 0;
    } else if (c == '\b') {
        if (cur_x > 0) cur_x--;
        VGA_MEM[cur_y * VGA_COLS + cur_x] = (VGA_ATTR << 8) | ' ';
    } else {
        VGA_MEM[cur_y * VGA_COLS + cur_x] = (VGA_ATTR << 8) | (uint8_t)c;
        cur_x++;
    }
    if (cur_x >= VGA_COLS) { cur_x = 0; cur_y++; }
    if (cur_y >= VGA_ROWS) {
        for (int row = 0; row < VGA_ROWS - 1; ++row)
            for (int col = 0; col < VGA_COLS; ++col)
                VGA_MEM[row * VGA_COLS + col] = VGA_MEM[(row + 1) * VGA_COLS + col];
        for (int col = 0; col < VGA_COLS; ++col)
            VGA_MEM[(VGA_ROWS - 1) * VGA_COLS + col] = (VGA_ATTR << 8) | ' ';
        cur_y = VGA_ROWS - 1;
    }
}

static void vga_write(const char *s) { while (*s) vga_putc(*s++); }

static void prompt_print(void) {
    vga_putc('u');
    vga_putc('s');
    vga_putc('e');
    vga_putc('r');
    vga_putc('@');
    vga_putc('v');
    vga_putc('i');
    vga_putc('b');
    vga_putc('e');
    vga_putc('-');
    vga_putc('o');
    vga_putc('s');
    vga_putc(':');
    vga_putc('/');
    vga_putc(' ');
    vga_putc('%');
    vga_putc(' ');
}

static void echo_backspace(void) {
    vga_putc('\b');
    vga_putc(' ');
    vga_putc('\b');
}

static int is_help(const char *cmd) {
    return cmd[0] == 'h' && cmd[1] == 'e' && cmd[2] == 'l' && cmd[3] == 'p' && cmd[4] == '\0';
}

static int is_echo(const char *cmd) {
    return cmd[0] == 'e' && cmd[1] == 'c' && cmd[2] == 'h' && cmd[3] == 'o' && cmd[4] == '\0';
}

static int is_clear(const char *cmd) {
    return cmd[0] == 'c' && cmd[1] == 'l' && cmd[2] == 'e' && cmd[3] == 'a' && cmd[4] == 'r' && cmd[5] == '\0';
}

static int is_exit(const char *cmd) {
    return cmd[0] == 'e' && cmd[1] == 'x' && cmd[2] == 'i' && cmd[3] == 't' && cmd[4] == '\0';
}

static void print_help(void) {
    vga_putc('h');
    vga_putc('e');
    vga_putc('l');
    vga_putc('p');
    vga_putc(',');
    vga_putc(' ');
    vga_putc('e');
    vga_putc('c');
    vga_putc('h');
    vga_putc('o');
    vga_putc(',');
    vga_putc(' ');
    vga_putc('c');
    vga_putc('l');
    vga_putc('e');
    vga_putc('a');
    vga_putc('r');
    vga_putc(',');
    vga_putc(' ');
    vga_putc('e');
    vga_putc('x');
    vga_putc('i');
    vga_putc('t');
    vga_putc('\n');
}

static void print_unknown(void) {
    vga_putc('u');
    vga_putc('n');
    vga_putc('k');
    vga_putc('n');
    vga_putc('o');
    vga_putc('w');
    vga_putc('n');
    vga_putc('\n');
}

static void print_bye(void) {
    vga_putc('b');
    vga_putc('y');
    vga_putc('e');
    vga_putc('\n');
}

static int read_line(char *buf, int maxlen) {
    int len = 0;
    
    for (;;) {
        int c;

        __asm__ volatile("cli" : : : "memory");
        c = kernel_keyboard_read();
        __asm__ volatile("sti" : : : "memory");

        if (c == 0) {
            yield();
            continue;
        }
        
        if (c == '\r') c = '\n';
        if (c == '\n') {
            vga_putc('\n');
            buf[len] = '\0';
            shell_state = 3;
            shell_last_len = (uint32_t)len;
            return len;
        }
        if ((c == '\b' || c == 127) && len > 0) {
            --len;
            echo_backspace();
            continue;
        }
        if (c >= 32 && c < 127 && len < maxlen - 1) {
            buf[len++] = (char)c;
            vga_putc((char)c);
        }
    }
}

void shell_start(void) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; ++i)
        VGA_MEM[i] = (VGA_ATTR << 8) | ' ';
    cur_x = cur_y = 0;
    shell_state = 1;

    char line[LINE_MAX];

    for (;;) {
        prompt_print();
        shell_state = 2;
        int len = read_line(line, LINE_MAX);
        shell_state = 4;
        if (len == 0) continue;

        /* split into cmd + arg remainder */
        char *p = line;
        while (*p == ' ') ++p;
        char *cmd = p;
        shell_last_cmd0 = (uint32_t)(uint8_t)cmd[0];
        while (*p && *p != ' ') ++p;
        if (*p) *p++ = '\0';
        while (*p == ' ') ++p;
        char *arg = p;

        if (is_help(cmd)) {
            shell_state = 5;
            print_help();
        } else if (is_echo(cmd)) {
            shell_state = 6;
            vga_write(arg);
            vga_putc('\n');
        } else if (is_clear(cmd)) {
            shell_state = 7;
            for (int i = 0; i < VGA_COLS * VGA_ROWS; ++i)
                VGA_MEM[i] = (VGA_ATTR << 8) | ' ';
            cur_x = cur_y = 0;
        } else if (is_exit(cmd)) {
            shell_state = 8;
            break;
        } else {
            shell_state = 9;
            print_unknown();
        }
    }
    shell_state = 10;
    print_bye();
}
