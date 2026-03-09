#include <stddef.h>
#include <stdint.h>

#include "userland_api.h"

#define FB_WIDTH 320
#define FB_HEIGHT 200
#define USERLAND_LOAD_ADDR 0x20000u
#define IDT_ENTRIES 256
#define IRQ0_VECTOR 0x20
#define IRQ1_VECTOR 0x21
#define IRQ12_VECTOR 0x2C
#define SYSCALL_VECTOR 0x80
#define KBD_QUEUE_SIZE 128

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct syscall_regs {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
};

static volatile uint8_t *const VGA_FB = (uint8_t *)0xA0000;
static struct idt_entry g_idt[IDT_ENTRIES];
static struct idt_ptr g_idt_ptr;

static volatile struct mouse_state g_mouse = {FB_WIDTH / 2, FB_HEIGHT / 2, 0};
static volatile uint8_t g_mouse_packet[3];
static volatile uint8_t g_mouse_packet_index = 0;
static volatile uint8_t g_mouse_updated = 0;
static volatile uint8_t g_mouse_ready = 0;

static volatile char g_kbd_queue[KBD_QUEUE_SIZE];
static volatile uint8_t g_kbd_head = 0;
static volatile uint8_t g_kbd_tail = 0;
static volatile uint8_t g_kbd_shift = 0;
static volatile uint8_t g_kbd_extended = 0;
static volatile uint32_t g_ticks = 0;

extern const uint8_t _binary_userland_bin_start[];
extern const uint8_t _binary_userland_bin_end[];
extern void irq0_stub(void);
extern void irq1_stub(void);
extern void irq12_stub(void);
extern void syscall_stub(void);

static const char kbd_map[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t', [0x10] = 'q',
    [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't', [0x15] = 'y',
    [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p', [0x1A] = '[',
    [0x1B] = ']', [0x1C] = '\n', [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd',
    [0x21] = 'f', [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`', [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' '
};

static const char kbd_shift_map[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x0E] = '\b', [0x0F] = '\t', [0x10] = 'Q',
    [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y',
    [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P', [0x1A] = '{',
    [0x1B] = '}', [0x1C] = '\n', [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D',
    [0x21] = 'F', [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~', [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x39] = ' '
};

static uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t irq_save(void) {
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void irq_restore(uint32_t flags) {
    __asm__ volatile("push %0; popf" : : "r"(flags) : "memory", "cc");
}

static void interrupts_enable(void) {
    __asm__ volatile("sti");
}

static void io_wait(void) {
    outb(0x80, 0);
}

static void fb_clear(uint8_t color) {
    for (size_t i = 0; i < (FB_WIDTH * FB_HEIGHT); ++i) {
        VGA_FB[i] = color;
    }
}

static void fb_draw_rect(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) {
        return;
    }

    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > FB_WIDTH) {
        x1 = FB_WIDTH;
    }
    if (y1 > FB_HEIGHT) {
        y1 = FB_HEIGHT;
    }

    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            VGA_FB[(py * FB_WIDTH) + px] = color;
        }
    }
}

static void memory_copy(uint8_t *dst, const uint8_t *src, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = src[i];
    }
}

static char uppercase_char(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    return c;
}

static uint8_t font_row_bits(char c, int row) {
    c = uppercase_char(c);

    switch (c) {
        case 'A': {
            static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
            return g[row];
        }
        case 'B': {
            static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
            return g[row];
        }
        case 'C': {
            static const uint8_t g[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
            return g[row];
        }
        case 'D': {
            static const uint8_t g[7] = {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C};
            return g[row];
        }
        case 'E': {
            static const uint8_t g[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
            return g[row];
        }
        case 'F': {
            static const uint8_t g[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
            return g[row];
        }
        case 'G': {
            static const uint8_t g[7] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
            return g[row];
        }
        case 'H': {
            static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
            return g[row];
        }
        case 'I': {
            static const uint8_t g[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
            return g[row];
        }
        case 'J': {
            static const uint8_t g[7] = {0x1F, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
            return g[row];
        }
        case 'K': {
            static const uint8_t g[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
            return g[row];
        }
        case 'L': {
            static const uint8_t g[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
            return g[row];
        }
        case 'M': {
            static const uint8_t g[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
            return g[row];
        }
        case 'N': {
            static const uint8_t g[7] = {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11};
            return g[row];
        }
        case 'O': {
            static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
            return g[row];
        }
        case 'P': {
            static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
            return g[row];
        }
        case 'Q': {
            static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
            return g[row];
        }
        case 'R': {
            static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
            return g[row];
        }
        case 'S': {
            static const uint8_t g[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
            return g[row];
        }
        case 'T': {
            static const uint8_t g[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
            return g[row];
        }
        case 'U': {
            static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
            return g[row];
        }
        case 'V': {
            static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
            return g[row];
        }
        case 'W': {
            static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
            return g[row];
        }
        case 'X': {
            static const uint8_t g[7] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
            return g[row];
        }
        case 'Y': {
            static const uint8_t g[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
            return g[row];
        }
        case 'Z': {
            static const uint8_t g[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
            return g[row];
        }
        case '0': {
            static const uint8_t g[7] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
            return g[row];
        }
        case '1': {
            static const uint8_t g[7] = {0x04, 0x0C, 0x14, 0x04, 0x04, 0x04, 0x1F};
            return g[row];
        }
        case '2': {
            static const uint8_t g[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
            return g[row];
        }
        case '3': {
            static const uint8_t g[7] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
            return g[row];
        }
        case '4': {
            static const uint8_t g[7] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
            return g[row];
        }
        case '5': {
            static const uint8_t g[7] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
            return g[row];
        }
        case '6': {
            static const uint8_t g[7] = {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
            return g[row];
        }
        case '7': {
            static const uint8_t g[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
            return g[row];
        }
        case '8': {
            static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
            return g[row];
        }
        case '9': {
            static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
            return g[row];
        }
        case '>': {
            static const uint8_t g[7] = {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10};
            return g[row];
        }
        case '<': {
            static const uint8_t g[7] = {0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01};
            return g[row];
        }
        case ':': {
            static const uint8_t g[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
            return g[row];
        }
        case '-': {
            static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
            return g[row];
        }
        case '_': {
            static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};
            return g[row];
        }
        case '.': {
            static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
            return g[row];
        }
        case '/': {
            static const uint8_t g[7] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00};
            return g[row];
        }
        case '\\': {
            static const uint8_t g[7] = {0x10, 0x08, 0x04, 0x02, 0x01, 0x00, 0x00};
            return g[row];
        }
        case '[': {
            static const uint8_t g[7] = {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E};
            return g[row];
        }
        case ']': {
            static const uint8_t g[7] = {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E};
            return g[row];
        }
        case '=': {
            static const uint8_t g[7] = {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00};
            return g[row];
        }
        case '+': {
            static const uint8_t g[7] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
            return g[row];
        }
        case '(': {
            static const uint8_t g[7] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
            return g[row];
        }
        case ')': {
            static const uint8_t g[7] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
            return g[row];
        }
        case '?': {
            static const uint8_t g[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
            return g[row];
        }
        case '!': {
            static const uint8_t g[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
            return g[row];
        }
        case ' ': return 0x00;
        default: {
            static const uint8_t g[7] = {0x1F, 0x01, 0x05, 0x09, 0x11, 0x00, 0x11};
            return g[row];
        }
    }
}

static void fb_draw_char(int x, int y, char c, uint8_t color) {
    for (int row = 0; row < 7; ++row) {
        const uint8_t bits = font_row_bits(c, row);
        for (int col = 0; col < 5; ++col) {
            if ((bits & (1u << (4 - col))) == 0u) {
                continue;
            }
            const int px = x + col;
            const int py = y + row;
            if (px < 0 || py < 0 || px >= FB_WIDTH || py >= FB_HEIGHT) {
                continue;
            }
            VGA_FB[(py * FB_WIDTH) + px] = color;
        }
    }
}

static void fb_draw_text(int x, int y, const char *text, uint8_t color) {
    int cx = x;
    int cy = y;

    while (*text != '\0') {
        const char c = *text++;
        if (c == '\n') {
            cx = x;
            cy += 8;
            continue;
        }

        fb_draw_char(cx, cy, c, color);
        cx += 6;
    }
}

static void idt_set_gate(uint8_t vector, uint32_t handler, uint8_t type_attr) {
    g_idt[vector].offset_low = (uint16_t)(handler & 0xFFFFu);
    g_idt[vector].selector = 0x08;
    g_idt[vector].zero = 0;
    g_idt[vector].type_attr = type_attr;
    g_idt[vector].offset_high = (uint16_t)((handler >> 16) & 0xFFFFu);
}

static void idt_init(void) {
    for (size_t i = 0; i < IDT_ENTRIES; ++i) {
        g_idt[i].offset_low = 0;
        g_idt[i].selector = 0;
        g_idt[i].zero = 0;
        g_idt[i].type_attr = 0;
        g_idt[i].offset_high = 0;
    }

    idt_set_gate(IRQ0_VECTOR, (uint32_t)(uintptr_t)irq0_stub, 0x8E);
    idt_set_gate(IRQ1_VECTOR, (uint32_t)(uintptr_t)irq1_stub, 0x8E);
    idt_set_gate(IRQ12_VECTOR, (uint32_t)(uintptr_t)irq12_stub, 0x8E);
    idt_set_gate(SYSCALL_VECTOR, (uint32_t)(uintptr_t)syscall_stub, 0x8F);

    g_idt_ptr.limit = (uint16_t)(sizeof(g_idt) - 1u);
    g_idt_ptr.base = (uint32_t)(uintptr_t)&g_idt[0];
    __asm__ volatile("lidt %0" : : "m"(g_idt_ptr));
}

static void pic_send_eoi(uint8_t irq_line) {
    if (irq_line >= 8) {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);
}

static void pic_remap(void) {
    const uint8_t mask_master = 0xFF;
    const uint8_t mask_slave = 0xFF;

    outb(0x20, 0x11);
    io_wait();
    outb(0xA0, 0x11);
    io_wait();

    outb(0x21, 0x20);
    io_wait();
    outb(0xA1, 0x28);
    io_wait();

    outb(0x21, 0x04);
    io_wait();
    outb(0xA1, 0x02);
    io_wait();

    outb(0x21, 0x01);
    io_wait();
    outb(0xA1, 0x01);
    io_wait();

    outb(0x21, mask_master);
    outb(0xA1, mask_slave);
}

static void pic_unmask_irq(uint8_t irq_line) {
    if (irq_line < 8) {
        uint8_t mask = inb(0x21);
        mask &= (uint8_t)~(1u << irq_line);
        outb(0x21, mask);
        return;
    }

    irq_line -= 8;
    uint8_t mask = inb(0xA1);
    mask &= (uint8_t)~(1u << irq_line);
    outb(0xA1, mask);
}

static void pit_init(uint32_t freq_hz) {
    uint32_t divisor;

    if (freq_hz == 0u) {
        freq_hz = 100u;
    }

    divisor = 1193182u / freq_hz;
    if (divisor == 0u) {
        divisor = 1u;
    }
    if (divisor > 0xFFFFu) {
        divisor = 0xFFFFu;
    }

    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFFu));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFFu));
}

static void ps2_wait_write(void) {
    while ((inb(0x64) & 0x02u) != 0u) {
    }
}

static int ps2_wait_read_timeout(void) {
    for (uint32_t i = 0; i < 1000000u; ++i) {
        if ((inb(0x64) & 0x01u) != 0u) {
            return 1;
        }
    }
    return 0;
}

static void ps2_drain_output(void) {
    while ((inb(0x64) & 0x01u) != 0u) {
        (void)inb(0x60);
    }
}

static void mouse_write_cmd(uint8_t value) {
    ps2_wait_write();
    outb(0x64, 0xD4);
    ps2_wait_write();
    outb(0x60, value);
}

static int mouse_expect_ack(void) {
    if (!ps2_wait_read_timeout()) {
        return 0;
    }
    return inb(0x60) == 0xFA;
}

static int mouse_hw_init(void) {
    uint8_t config;

    ps2_drain_output();

    ps2_wait_write();
    outb(0x64, 0xA8);

    ps2_wait_write();
    outb(0x64, 0x20);
    if (!ps2_wait_read_timeout()) {
        return 0;
    }

    config = inb(0x60);
    config |= 0x02u;
    config &= (uint8_t)~0x20u;

    ps2_wait_write();
    outb(0x64, 0x60);
    ps2_wait_write();
    outb(0x60, config);

    mouse_write_cmd(0xF6);
    if (!mouse_expect_ack()) {
        return 0;
    }

    mouse_write_cmd(0xF4);
    if (!mouse_expect_ack()) {
        return 0;
    }

    g_mouse_packet_index = 0;
    return 1;
}

static void kbd_push_char(char c) {
    const uint8_t next = (uint8_t)((g_kbd_head + 1u) % KBD_QUEUE_SIZE);
    if (next == g_kbd_tail) {
        return;
    }
    g_kbd_queue[g_kbd_head] = c;
    g_kbd_head = next;
}

static int kbd_pop_char(void) {
    uint32_t flags = irq_save();
    int value = 0;

    if (g_kbd_tail != g_kbd_head) {
        value = (uint8_t)g_kbd_queue[g_kbd_tail];
        g_kbd_tail = (uint8_t)((g_kbd_tail + 1u) % KBD_QUEUE_SIZE);
    }

    irq_restore(flags);
    return value;
}

void keyboard_irq_handler_c(void) {
    const uint8_t scancode = inb(0x60);

    if (scancode == 0xE0u) {
        g_kbd_extended = 1;
        pic_send_eoi(1);
        return;
    }

    if (g_kbd_extended) {
        g_kbd_extended = 0;
        pic_send_eoi(1);
        return;
    }

    if (scancode == 0x2Au || scancode == 0x36u) {
        g_kbd_shift = 1;
        pic_send_eoi(1);
        return;
    }

    if (scancode == 0xAAu || scancode == 0xB6u) {
        g_kbd_shift = 0;
        pic_send_eoi(1);
        return;
    }

    if ((scancode & 0x80u) != 0u) {
        pic_send_eoi(1);
        return;
    }

    const char c = g_kbd_shift ? kbd_shift_map[scancode] : kbd_map[scancode];
    if (c != '\0') {
        kbd_push_char(c);
    }

    pic_send_eoi(1);
}

void timer_irq_handler_c(void) {
    g_ticks += 1u;
    pic_send_eoi(0);
}

void mouse_irq_handler_c(void) {
    const uint8_t data = inb(0x60);

    if (!g_mouse_ready) {
        pic_send_eoi(12);
        return;
    }

    if (g_mouse_packet_index == 0 && (data & 0x08u) == 0u) {
        pic_send_eoi(12);
        return;
    }

    g_mouse_packet[g_mouse_packet_index++] = data;
    if (g_mouse_packet_index < 3) {
        pic_send_eoi(12);
        return;
    }

    g_mouse_packet_index = 0;

    g_mouse.x += (int8_t)g_mouse_packet[1];
    g_mouse.y -= (int8_t)g_mouse_packet[2];

    if (g_mouse.x < 0) {
        g_mouse.x = 0;
    } else if (g_mouse.x >= FB_WIDTH) {
        g_mouse.x = FB_WIDTH - 1;
    }

    if (g_mouse.y < 0) {
        g_mouse.y = 0;
    } else if (g_mouse.y >= FB_HEIGHT) {
        g_mouse.y = FB_HEIGHT - 1;
    }

    g_mouse.buttons = g_mouse_packet[0] & 0x07u;
    g_mouse_updated = 1;

    pic_send_eoi(12);
}

void syscall_dispatch_c(struct syscall_regs *regs) {
    switch (regs->eax) {
        case SYSCALL_GFX_CLEAR:
            fb_clear((uint8_t)(regs->ebx & 0xFFu));
            regs->eax = 0;
            break;

        case SYSCALL_GFX_RECT:
            fb_draw_rect((int)regs->ebx, (int)regs->ecx, (int)regs->edx, (int)regs->esi,
                         (uint8_t)(regs->edi & 0xFFu));
            regs->eax = 0;
            break;

        case SYSCALL_GFX_TEXT:
            fb_draw_text((int)regs->ebx, (int)regs->ecx, (const char *)(uintptr_t)regs->edx,
                         (uint8_t)(regs->esi & 0xFFu));
            regs->eax = 0;
            break;

        case SYSCALL_INPUT_MOUSE: {
            struct mouse_state *out_state = (struct mouse_state *)(uintptr_t)regs->ebx;
            uint32_t flags = irq_save();
            if (out_state != NULL) {
                out_state->x = g_mouse.x;
                out_state->y = g_mouse.y;
                out_state->buttons = g_mouse.buttons;
            }
            regs->eax = g_mouse_updated ? 1u : 0u;
            g_mouse_updated = 0;
            irq_restore(flags);
            break;
        }

        case SYSCALL_INPUT_KEY:
            regs->eax = (uint32_t)kbd_pop_char();
            break;

        case SYSCALL_SLEEP:
            __asm__ volatile("hlt");
            regs->eax = 0;
            break;

        case SYSCALL_TIME_TICKS: {
            uint32_t flags = irq_save();
            regs->eax = g_ticks;
            irq_restore(flags);
            break;
        }

        default:
            regs->eax = (uint32_t)-1;
            break;
    }
}

static void run_userland(void) {
    const size_t userland_size =
        (size_t)(_binary_userland_bin_end - _binary_userland_bin_start);
    uint8_t *const load_addr = (uint8_t *)USERLAND_LOAD_ADDR;

    if (userland_size == 0) {
        fb_clear(0x04);
        return;
    }

    memory_copy(load_addr, _binary_userland_bin_start, userland_size);
    ((userland_entry_t)load_addr)();
}

__attribute__((noreturn, section(".entry"))) void stage2_entry(void) {
    fb_clear(0x01);

    idt_init();
    pic_remap();
    pit_init(100u);
    pic_unmask_irq(0);
    pic_unmask_irq(1);
    pic_unmask_irq(2);
    pic_unmask_irq(12);

    if (mouse_hw_init()) {
        g_mouse_ready = 1;
    } else {
        fb_draw_rect(0, 0, FB_WIDTH, 12, 0x04);
    }

    interrupts_enable();
    run_userland();

    for (;;) {
        __asm__ volatile("hlt");
    }
}
