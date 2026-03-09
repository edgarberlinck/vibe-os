#include <stdint.h>

#include "userland_api.h"

#define SCREEN_W 320
#define SCREEN_H 200

#define TERM_COLS 30
#define TERM_ROWS 14
#define INPUT_MAX 46

#define FS_MAX_NODES 64
#define FS_NAME_MAX 15
#define FS_FILE_MAX 120
#define FS_MAX_SEGMENTS 8

struct rect {
    int x;
    int y;
    int w;
    int h;
};

struct fs_node {
    int used;
    int is_dir;
    int parent;
    int first_child;
    int next_sibling;
    char name[FS_NAME_MAX + 1];
    int size;
    char data[FS_FILE_MAX + 1];
};

static const struct rect g_terminal_window = {10, 24, 196, 150};
static const struct rect g_clock_window = {212, 28, 98, 68};

static char g_term_lines[TERM_ROWS][TERM_COLS + 1];
static int g_term_line_count = 0;
static char g_input[INPUT_MAX + 1];
static int g_input_len = 0;

static struct fs_node g_fs_nodes[FS_MAX_NODES];
static int g_fs_root = -1;
static int g_fs_cwd = -1;

static inline int syscall5(int num, int a, int b, int c, int d, int e) {
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                     : "memory", "cc");
    return ret;
}

static void sys_clear(uint8_t color) {
    (void)syscall5(SYSCALL_GFX_CLEAR, color, 0, 0, 0, 0);
}

static void sys_rect(int x, int y, int w, int h, uint8_t color) {
    (void)syscall5(SYSCALL_GFX_RECT, x, y, w, h, color);
}

static void sys_text(int x, int y, uint8_t color, const char *text) {
    (void)syscall5(SYSCALL_GFX_TEXT, x, y, (int)(uintptr_t)text, color, 0);
}

static int sys_poll_mouse(struct mouse_state *state) {
    return syscall5(SYSCALL_INPUT_MOUSE, (int)(uintptr_t)state, 0, 0, 0, 0);
}

static int sys_poll_key(void) {
    return syscall5(SYSCALL_INPUT_KEY, 0, 0, 0, 0, 0);
}

static void sys_sleep(void) {
    (void)syscall5(SYSCALL_SLEEP, 0, 0, 0, 0, 0);
}

static uint32_t sys_ticks(void) {
    return (uint32_t)syscall5(SYSCALL_TIME_TICKS, 0, 0, 0, 0, 0);
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

static int str_eq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int to_upper(int c) {
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 'A';
    }
    return c;
}

static int str_eq_ci(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (to_upper(*a) != to_upper(*b)) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void str_copy_limited(char *dst, const char *src, int max_len) {
    int i = 0;
    while (src[i] != '\0' && i < (max_len - 1)) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void str_append(char *dst, const char *src, int max_len) {
    int len = str_len(dst);
    int i = 0;

    while (src[i] != '\0' && (len + i) < (max_len - 1)) {
        dst[len + i] = src[i];
        ++i;
    }
    dst[len + i] = '\0';
}

static char *skip_spaces(char *s) {
    while (*s == ' ') {
        ++s;
    }
    return s;
}

static char *next_token(char **cursor) {
    char *start = skip_spaces(*cursor);
    char *p;

    if (*start == '\0') {
        *cursor = start;
        return 0;
    }

    p = start;
    while (*p != '\0' && *p != ' ') {
        ++p;
    }

    if (*p != '\0') {
        *p = '\0';
        ++p;
    }

    *cursor = p;
    return start;
}

static int point_in_rect(const struct rect *r, int x, int y) {
    return x >= r->x && x < (r->x + r->w) && y >= r->y && y < (r->y + r->h);
}

static struct rect window_close_button(const struct rect *w) {
    struct rect close = {w->x + w->w - 14, w->y + 2, 10, 10};
    return close;
}

static void terminal_push_line(const char *text) {
    int i;
    int n = str_len(text);

    if (g_term_line_count == TERM_ROWS) {
        for (i = 1; i < TERM_ROWS; ++i) {
            int j = 0;
            while (g_term_lines[i][j] != '\0') {
                g_term_lines[i - 1][j] = g_term_lines[i][j];
                ++j;
            }
            g_term_lines[i - 1][j] = '\0';
        }
        g_term_line_count = TERM_ROWS - 1;
    }

    if (n > TERM_COLS) {
        n = TERM_COLS;
    }
    for (i = 0; i < n; ++i) {
        g_term_lines[g_term_line_count][i] = text[i];
    }
    g_term_lines[g_term_line_count][n] = '\0';
    ++g_term_line_count;
}

static void terminal_clear_lines(void) {
    g_term_line_count = 0;
}

static void fs_reset_node(int idx) {
    int i;

    g_fs_nodes[idx].used = 0;
    g_fs_nodes[idx].is_dir = 0;
    g_fs_nodes[idx].parent = -1;
    g_fs_nodes[idx].first_child = -1;
    g_fs_nodes[idx].next_sibling = -1;
    g_fs_nodes[idx].name[0] = '\0';
    g_fs_nodes[idx].size = 0;

    for (i = 0; i <= FS_FILE_MAX; ++i) {
        g_fs_nodes[idx].data[i] = '\0';
    }
}

static int fs_alloc_node(void) {
    int i;
    for (i = 0; i < FS_MAX_NODES; ++i) {
        if (!g_fs_nodes[i].used) {
            return i;
        }
    }
    return -1;
}

static int fs_find_child(int parent, const char *name) {
    int child = g_fs_nodes[parent].first_child;

    while (child != -1) {
        if (g_fs_nodes[child].used && str_eq(g_fs_nodes[child].name, name)) {
            return child;
        }
        child = g_fs_nodes[child].next_sibling;
    }

    return -1;
}

static void fs_link_child(int parent, int child) {
    g_fs_nodes[child].next_sibling = g_fs_nodes[parent].first_child;
    g_fs_nodes[parent].first_child = child;
    g_fs_nodes[child].parent = parent;
}

static void fs_unlink_child(int parent, int child) {
    int cur = g_fs_nodes[parent].first_child;
    int prev = -1;

    while (cur != -1) {
        if (cur == child) {
            if (prev == -1) {
                g_fs_nodes[parent].first_child = g_fs_nodes[cur].next_sibling;
            } else {
                g_fs_nodes[prev].next_sibling = g_fs_nodes[cur].next_sibling;
            }
            g_fs_nodes[cur].next_sibling = -1;
            return;
        }
        prev = cur;
        cur = g_fs_nodes[cur].next_sibling;
    }
}

static int fs_has_children(int idx) {
    return g_fs_nodes[idx].first_child != -1;
}

static int fs_new_node(const char *name, int is_dir, int parent) {
    int idx = fs_alloc_node();
    if (idx < 0) {
        return -1;
    }

    g_fs_nodes[idx].used = 1;
    g_fs_nodes[idx].is_dir = is_dir;
    g_fs_nodes[idx].parent = parent;
    g_fs_nodes[idx].first_child = -1;
    g_fs_nodes[idx].next_sibling = -1;
    g_fs_nodes[idx].size = 0;
    g_fs_nodes[idx].data[0] = '\0';
    str_copy_limited(g_fs_nodes[idx].name, name, FS_NAME_MAX + 1);

    if (parent >= 0) {
        fs_link_child(parent, idx);
    }

    return idx;
}

static int fs_split_path(const char *path,
                         char segments[FS_MAX_SEGMENTS][FS_NAME_MAX + 1],
                         int *is_abs) {
    int count = 0;
    const char *p = path;

    *is_abs = 0;
    if (*p == '/') {
        *is_abs = 1;
        while (*p == '/') {
            ++p;
        }
    }

    while (*p != '\0') {
        int len = 0;

        if (count >= FS_MAX_SEGMENTS) {
            return -1;
        }

        while (*p != '\0' && *p != '/') {
            if (len < FS_NAME_MAX) {
                segments[count][len++] = *p;
            }
            ++p;
        }
        segments[count][len] = '\0';
        ++count;

        while (*p == '/') {
            ++p;
        }
    }

    return count;
}

static int fs_resolve(const char *path) {
    char seg[FS_MAX_SEGMENTS][FS_NAME_MAX + 1];
    int is_abs = 0;
    int count;
    int cur;
    int i;

    if (path == 0 || path[0] == '\0') {
        return g_fs_cwd;
    }

    count = fs_split_path(path, seg, &is_abs);
    if (count < 0) {
        return -1;
    }

    cur = is_abs ? g_fs_root : g_fs_cwd;
    if (count == 0) {
        return cur;
    }

    for (i = 0; i < count; ++i) {
        if (str_eq(seg[i], ".") || seg[i][0] == '\0') {
            continue;
        }

        if (str_eq(seg[i], "..")) {
            if (cur != g_fs_root) {
                cur = g_fs_nodes[cur].parent;
            }
            continue;
        }

        {
            int child = fs_find_child(cur, seg[i]);
            if (child < 0) {
                return -1;
            }
            if (i < (count - 1) && !g_fs_nodes[child].is_dir) {
                return -1;
            }
            cur = child;
        }
    }

    return cur;
}

static int fs_resolve_parent(const char *path, int *parent_out, char *name_out) {
    char seg[FS_MAX_SEGMENTS][FS_NAME_MAX + 1];
    int is_abs = 0;
    int count;
    int cur;
    int i;

    if (path == 0 || path[0] == '\0') {
        return -1;
    }

    count = fs_split_path(path, seg, &is_abs);
    if (count <= 0) {
        return -1;
    }

    cur = is_abs ? g_fs_root : g_fs_cwd;
    for (i = 0; i < (count - 1); ++i) {
        if (str_eq(seg[i], ".") || seg[i][0] == '\0') {
            continue;
        }

        if (str_eq(seg[i], "..")) {
            if (cur != g_fs_root) {
                cur = g_fs_nodes[cur].parent;
            }
            continue;
        }

        {
            int child = fs_find_child(cur, seg[i]);
            if (child < 0 || !g_fs_nodes[child].is_dir) {
                return -1;
            }
            cur = child;
        }
    }

    if (str_eq(seg[count - 1], ".") || str_eq(seg[count - 1], "..") ||
        seg[count - 1][0] == '\0') {
        return -1;
    }

    *parent_out = cur;
    str_copy_limited(name_out, seg[count - 1], FS_NAME_MAX + 1);
    return 0;
}

static int fs_create(const char *path, int is_dir) {
    int parent;
    char name[FS_NAME_MAX + 1];

    if (fs_resolve_parent(path, &parent, name) != 0) {
        return -1;
    }

    if (!g_fs_nodes[parent].is_dir) {
        return -1;
    }

    if (fs_find_child(parent, name) >= 0) {
        return -2;
    }

    return fs_new_node(name, is_dir, parent) >= 0 ? 0 : -3;
}

static int fs_remove(const char *path) {
    int idx = fs_resolve(path);
    int parent;

    if (idx < 0 || idx == g_fs_root) {
        return -1;
    }

    if (g_fs_nodes[idx].is_dir && fs_has_children(idx)) {
        return -2;
    }

    parent = g_fs_nodes[idx].parent;
    fs_unlink_child(parent, idx);
    fs_reset_node(idx);
    return 0;
}

static int fs_write_file(const char *path, const char *text, int append) {
    int idx = fs_resolve(path);
    int i;

    if (idx < 0) {
        if (fs_create(path, 0) != 0) {
            return -1;
        }
        idx = fs_resolve(path);
        if (idx < 0) {
            return -1;
        }
    }

    if (g_fs_nodes[idx].is_dir) {
        return -2;
    }

    if (!append) {
        g_fs_nodes[idx].size = 0;
        g_fs_nodes[idx].data[0] = '\0';
    }

    i = g_fs_nodes[idx].size;
    while (*text != '\0' && i < FS_FILE_MAX) {
        g_fs_nodes[idx].data[i++] = *text++;
    }
    g_fs_nodes[idx].data[i] = '\0';
    g_fs_nodes[idx].size = i;
    return 0;
}

static void fs_build_path(int node, char *out, int max_len) {
    int stack[FS_MAX_NODES];
    int top = 0;
    int i;
    int pos = 0;

    if (node == g_fs_root) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }

    while (node != g_fs_root && node >= 0 && top < FS_MAX_NODES) {
        stack[top++] = node;
        node = g_fs_nodes[node].parent;
    }

    out[pos++] = '/';
    for (i = top - 1; i >= 0; --i) {
        const char *name = g_fs_nodes[stack[i]].name;
        while (*name != '\0' && pos < (max_len - 1)) {
            out[pos++] = *name++;
        }
        if (i > 0 && pos < (max_len - 1)) {
            out[pos++] = '/';
        }
    }
    out[pos] = '\0';
}

static void fs_init(void) {
    int i;

    for (i = 0; i < FS_MAX_NODES; ++i) {
        fs_reset_node(i);
    }

    g_fs_root = fs_new_node("", 1, -1);
    g_fs_nodes[g_fs_root].parent = g_fs_root;
    g_fs_cwd = g_fs_root;

    (void)fs_create("/home", 1);
    (void)fs_create("/home/user", 1);
    (void)fs_create("/docs", 1);
    (void)fs_write_file("/README", "SISTEMA DE ARQUIVOS VFS", 0);
}

static void fs_cmd_pwd(void) {
    char path[80];
    fs_build_path(g_fs_cwd, path, (int)sizeof(path));
    terminal_push_line(path);
}

static void fs_cmd_ls(const char *path) {
    int dir_idx = path && path[0] ? fs_resolve(path) : g_fs_cwd;
    int child;

    if (dir_idx < 0 || !g_fs_nodes[dir_idx].is_dir) {
        terminal_push_line("ERRO: DIRETORIO NAO ENCONTRADO");
        return;
    }

    child = g_fs_nodes[dir_idx].first_child;
    if (child == -1) {
        terminal_push_line("(VAZIO)");
        return;
    }

    while (child != -1) {
        char line[TERM_COLS + 1];
        str_copy_limited(line, g_fs_nodes[child].name, (int)sizeof(line));
        if (g_fs_nodes[child].is_dir) {
            str_append(line, "/", (int)sizeof(line));
        }
        terminal_push_line(line);
        child = g_fs_nodes[child].next_sibling;
    }
}

static void fs_cmd_cd(const char *path) {
    int idx;
    if (path == 0 || path[0] == '\0') {
        g_fs_cwd = g_fs_root;
        return;
    }

    idx = fs_resolve(path);
    if (idx < 0 || !g_fs_nodes[idx].is_dir) {
        terminal_push_line("ERRO: DIRETORIO INVALIDO");
        return;
    }

    g_fs_cwd = idx;
}

static void fs_cmd_cat(const char *path) {
    int idx = fs_resolve(path);

    if (idx < 0 || g_fs_nodes[idx].is_dir) {
        terminal_push_line("ERRO: ARQUIVO NAO ENCONTRADO");
        return;
    }

    if (g_fs_nodes[idx].size == 0) {
        terminal_push_line("(ARQUIVO VAZIO)");
        return;
    }

    terminal_push_line(g_fs_nodes[idx].data);
}

static void terminal_show_help(void) {
    terminal_push_line("COMANDOS:");
    terminal_push_line("HELP PWD LS CD");
    terminal_push_line("MKDIR TOUCH RM");
    terminal_push_line("CAT WRITE APPEND");
    terminal_push_line("CLEAR UNAME EXIT");
}

static void terminal_execute_command(int *terminal_open) {
    char line[INPUT_MAX + 1];
    char *cursor;
    char *cmd;
    int i = 0;

    while (i < g_input_len) {
        line[i] = g_input[i];
        ++i;
    }
    line[i] = '\0';

    {
        char cwd[52];
        char prompt_line[TERM_COLS + 1];
        fs_build_path(g_fs_cwd, cwd, (int)sizeof(cwd));
        prompt_line[0] = '\0';
        str_append(prompt_line, "[", (int)sizeof(prompt_line));
        str_append(prompt_line, cwd, (int)sizeof(prompt_line));
        str_append(prompt_line, "] ", (int)sizeof(prompt_line));
        str_append(prompt_line, line, (int)sizeof(prompt_line));
        terminal_push_line(prompt_line);
    }

    cursor = line;
    cmd = next_token(&cursor);
    if (!cmd) {
        g_input_len = 0;
        g_input[0] = '\0';
        return;
    }

    if (str_eq_ci(cmd, "HELP")) {
        terminal_show_help();
    } else if (str_eq_ci(cmd, "PWD")) {
        fs_cmd_pwd();
    } else if (str_eq_ci(cmd, "LS")) {
        char *path = next_token(&cursor);
        fs_cmd_ls(path);
    } else if (str_eq_ci(cmd, "CD")) {
        char *path = next_token(&cursor);
        fs_cmd_cd(path);
    } else if (str_eq_ci(cmd, "MKDIR")) {
        char *path = next_token(&cursor);
        if (!path) {
            terminal_push_line("USO: MKDIR <DIR>");
        } else if (fs_create(path, 1) == 0) {
            terminal_push_line("OK");
        } else {
            terminal_push_line("ERRO AO CRIAR DIRETORIO");
        }
    } else if (str_eq_ci(cmd, "TOUCH")) {
        char *path = next_token(&cursor);
        if (!path) {
            terminal_push_line("USO: TOUCH <ARQUIVO>");
        } else if (fs_create(path, 0) == 0) {
            terminal_push_line("OK");
        } else {
            terminal_push_line("ERRO AO CRIAR ARQUIVO");
        }
    } else if (str_eq_ci(cmd, "RM")) {
        char *path = next_token(&cursor);
        if (!path) {
            terminal_push_line("USO: RM <ARQUIVO|DIR>");
        } else {
            int rc = fs_remove(path);
            if (rc == 0) {
                terminal_push_line("OK");
            } else if (rc == -2) {
                terminal_push_line("ERRO: DIRETORIO NAO VAZIO");
            } else {
                terminal_push_line("ERRO AO REMOVER");
            }
        }
    } else if (str_eq_ci(cmd, "CAT")) {
        char *path = next_token(&cursor);
        if (!path) {
            terminal_push_line("USO: CAT <ARQUIVO>");
        } else {
            fs_cmd_cat(path);
        }
    } else if (str_eq_ci(cmd, "WRITE")) {
        char *path = next_token(&cursor);
        char *text = skip_spaces(cursor);
        if (!path || text[0] == '\0') {
            terminal_push_line("USO: WRITE <ARQ> <TEXTO>");
        } else if (fs_write_file(path, text, 0) == 0) {
            terminal_push_line("OK");
        } else {
            terminal_push_line("ERRO AO ESCREVER");
        }
    } else if (str_eq_ci(cmd, "APPEND")) {
        char *path = next_token(&cursor);
        char *text = skip_spaces(cursor);
        if (!path || text[0] == '\0') {
            terminal_push_line("USO: APPEND <ARQ> <TXT>");
        } else if (fs_write_file(path, text, 1) == 0) {
            terminal_push_line("OK");
        } else {
            terminal_push_line("ERRO AO ESCREVER");
        }
    } else if (str_eq_ci(cmd, "CLEAR")) {
        terminal_clear_lines();
    } else if (str_eq_ci(cmd, "UNAME")) {
        terminal_push_line("BOOTLOADERVIBE X86");
    } else if (str_eq_ci(cmd, "EXIT")) {
        terminal_push_line("TERMINAL ENCERRADO");
        *terminal_open = 0;
    } else {
        terminal_push_line("COMANDO DESCONHECIDO");
    }

    g_input_len = 0;
    g_input[0] = '\0';
}

static void draw_window_frame(const struct rect *w, const char *title, int close_hover) {
    const struct rect close = window_close_button(w);

    sys_rect(w->x, w->y, w->w, w->h, 8);
    sys_rect(w->x, w->y, w->w, 14, 7);
    sys_text(w->x + 6, w->y + 3, 0, title);

    sys_rect(close.x, close.y, close.w, close.h, close_hover ? 12 : 4);
    sys_text(close.x + 3, close.y + 2, 15, "X");
}

static void draw_terminal_window(int close_hover) {
    int i;
    const int text_x = g_terminal_window.x + 8;
    const int text_y = g_terminal_window.y + 22;

    draw_window_frame(&g_terminal_window, "TERMINAL", close_hover);
    sys_rect(g_terminal_window.x + 4, g_terminal_window.y + 18, g_terminal_window.w - 8,
             g_terminal_window.h - 22, 0);

    for (i = 0; i < g_term_line_count; ++i) {
        sys_text(text_x, text_y + (i * 8), 15, g_term_lines[i]);
    }

    {
        char input_line[INPUT_MAX + 4];
        int n = 0;
        input_line[n++] = '>';
        input_line[n++] = ' ';
        for (i = 0; i < g_input_len && n < (int)sizeof(input_line) - 1; ++i) {
            input_line[n++] = g_input[i];
        }
        input_line[n] = '\0';
        sys_text(text_x, g_terminal_window.y + g_terminal_window.h - 12, 14, input_line);
    }
}

static void clock_format(char out[9], uint32_t ticks) {
    const uint32_t total_seconds = ticks / 100u;
    const uint32_t h = (total_seconds / 3600u) % 24u;
    const uint32_t m = (total_seconds / 60u) % 60u;
    const uint32_t s = total_seconds % 60u;

    out[0] = (char)('0' + (h / 10u));
    out[1] = (char)('0' + (h % 10u));
    out[2] = ':';
    out[3] = (char)('0' + (m / 10u));
    out[4] = (char)('0' + (m % 10u));
    out[5] = ':';
    out[6] = (char)('0' + (s / 10u));
    out[7] = (char)('0' + (s % 10u));
    out[8] = '\0';
}

static void draw_clock_window(uint32_t ticks, int close_hover) {
    char time_text[9];

    draw_window_frame(&g_clock_window, "RELOGIO", close_hover);
    sys_rect(g_clock_window.x + 4, g_clock_window.y + 18, g_clock_window.w - 8, g_clock_window.h - 22,
             1);

    clock_format(time_text, ticks);
    sys_text(g_clock_window.x + 14, g_clock_window.y + 34, 15, time_text);
    sys_text(g_clock_window.x + 10, g_clock_window.y + 50, 14, "UPTIME");
}

static void draw_cursor(const struct mouse_state *mouse) {
    sys_rect(mouse->x - 1, mouse->y - 1, 3, 3, 15);
    sys_rect(mouse->x + 2, mouse->y, 4, 1, 15);
    sys_rect(mouse->x, mouse->y + 2, 1, 4, 15);
}

static void draw_desktop(const struct mouse_state *mouse,
                         uint32_t ticks,
                         int menu_open,
                         int terminal_open,
                         int clock_open,
                         int start_hover,
                         int terminal_item_hover,
                         int clock_item_hover,
                         int term_close_hover,
                         int clock_close_hover) {
    const struct rect taskbar = {0, 184, SCREEN_W, 16};
    const struct rect start_button = {4, 186, 48, 12};
    const struct rect menu_rect = {4, 136, 130, 46};
    const struct rect terminal_item = {8, 148, 122, 14};
    const struct rect clock_item = {8, 164, 122, 14};

    sys_clear(1);
    sys_rect(6, 6, 118, 12, 9);
    sys_text(10, 8, 15, "BOOTLOADER VIBE");

    if (terminal_open) {
        draw_terminal_window(term_close_hover);
    }
    if (clock_open) {
        draw_clock_window(ticks, clock_close_hover);
    }

    sys_rect(taskbar.x, taskbar.y, taskbar.w, taskbar.h, 8);
    sys_rect(start_button.x, start_button.y, start_button.w, start_button.h, start_hover ? 14 : 10);
    sys_text(start_button.x + 10, start_button.y + 3, 0, "START");

    if (menu_open) {
        sys_rect(menu_rect.x, menu_rect.y, menu_rect.w, menu_rect.h, 7);
        sys_text(menu_rect.x + 6, menu_rect.y + 4, 0, "APPS");

        sys_rect(terminal_item.x, terminal_item.y, terminal_item.w, terminal_item.h,
                 terminal_item_hover ? 14 : 9);
        sys_text(terminal_item.x + 6, terminal_item.y + 4, 0, "TERMINAL");

        sys_rect(clock_item.x, clock_item.y, clock_item.w, clock_item.h, clock_item_hover ? 14 : 9);
        sys_text(clock_item.x + 6, clock_item.y + 4, 0, "RELOGIO");
    }

    draw_cursor(mouse);
}

__attribute__((section(".entry"))) void userland_entry(void) {
    const struct rect start_button = {4, 186, 48, 12};
    const struct rect menu_rect = {4, 136, 130, 46};
    const struct rect terminal_item = {8, 148, 122, 14};
    const struct rect clock_item = {8, 164, 122, 14};
    const struct rect terminal_close = window_close_button(&g_terminal_window);
    const struct rect clock_close = window_close_button(&g_clock_window);

    struct mouse_state mouse = {SCREEN_W / 2, SCREEN_H / 2, 0};
    uint32_t last_clock_second = 0xFFFFFFFFu;
    int menu_open = 0;
    int terminal_open = 0;
    int clock_open = 1;
    int prev_left = 0;
    int dirty = 1;

    fs_init();
    terminal_push_line("SHELL INTERATIVO COM VFS");
    terminal_push_line("DIGITE HELP");

    for (;;) {
        int key;
        const uint32_t ticks = sys_ticks();
        const uint32_t second_now = ticks / 100u;
        const int mouse_event = sys_poll_mouse(&mouse);
        const int left_pressed = (mouse.buttons & 0x01u) != 0;
        const int start_hover = point_in_rect(&start_button, mouse.x, mouse.y);
        const int terminal_item_hover = menu_open && point_in_rect(&terminal_item, mouse.x, mouse.y);
        const int clock_item_hover = menu_open && point_in_rect(&clock_item, mouse.x, mouse.y);
        const int term_close_hover = terminal_open && point_in_rect(&terminal_close, mouse.x, mouse.y);
        const int clock_close_hover = clock_open && point_in_rect(&clock_close, mouse.x, mouse.y);

        if (mouse_event) {
            dirty = 1;
        }

        if (clock_open && second_now != last_clock_second) {
            last_clock_second = second_now;
            dirty = 1;
        }

        if (left_pressed && !prev_left) {
            if (start_hover) {
                menu_open = !menu_open;
                dirty = 1;
            } else if (menu_open && terminal_item_hover) {
                terminal_open = 1;
                menu_open = 0;
                dirty = 1;
            } else if (menu_open && clock_item_hover) {
                clock_open = 1;
                menu_open = 0;
                dirty = 1;
            } else if (term_close_hover) {
                terminal_open = 0;
                dirty = 1;
            } else if (clock_close_hover) {
                clock_open = 0;
                dirty = 1;
            } else if (menu_open && !point_in_rect(&menu_rect, mouse.x, mouse.y)) {
                menu_open = 0;
                dirty = 1;
            }
        }
        prev_left = left_pressed;

        while ((key = sys_poll_key()) != 0) {
            if (!terminal_open) {
                continue;
            }

            if (key == '\b') {
                if (g_input_len > 0) {
                    --g_input_len;
                    g_input[g_input_len] = '\0';
                    dirty = 1;
                }
                continue;
            }

            if (key == '\n') {
                terminal_execute_command(&terminal_open);
                dirty = 1;
                continue;
            }

            if (key >= 32 && key <= 126 && g_input_len < INPUT_MAX) {
                g_input[g_input_len++] = (char)key;
                g_input[g_input_len] = '\0';
                dirty = 1;
            }
        }

        if (dirty) {
            draw_desktop(&mouse, ticks, menu_open, terminal_open, clock_open, start_hover,
                         terminal_item_hover, clock_item_hover, term_close_hover, clock_close_hover);
            dirty = 0;
        }

        sys_sleep();
    }
}
