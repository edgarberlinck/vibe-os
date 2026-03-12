#include <kernel/kernel.h>  /* use include path */
#include <kernel/interrupt.h> /* new interrupt interfaces */
#include <kernel/scheduler.h>
#include <kernel/driver_manager.h>
#include <kernel/memory/memory_init.h>  /* kernel/memory via CFLAGS */

/* early kernel entry; mirrors original stage2 initialization.  Eventually
   this function will orchestrate the new modular subsystems. */

/* bring in existing stage2 APIs for the moment */
#include <stage2/include/video.h>   /* from stage2/include via CFLAGS */
#include <stage2/include/timer.h>
#include <stage2/include/keyboard.h>
#include <stage2/include/mouse.h>
#include <stage2/include/userland.h>

/* simple VGA text mode output for boot debugging */
static void vga_text_putc(char c) {
    static int x = 0, y = 0;
    volatile uint16_t *video_mem = (uint16_t *)0xB8000;
    
    if (c == '\n') {
        y++;
        x = 0;
        if (y >= 25) y = 0;
        return;
    }
    
    if (x >= 80) {
        x = 0;
        y++;
    }
    if (y >= 25) y = 0;
    
    int offset = y * 80 + x;
    video_mem[offset] = (0x0F << 8) | (uint8_t)c;
    x++;
}

static void vga_text_puts(const char *str) {
    for (const char *p = str; *p; p++) {
        vga_text_putc(*p);
    }
}

__attribute__((noreturn)) void kernel_entry(void) {
    /* clear screen first */
    volatile uint16_t *video_mem = (uint16_t *)0xB8000;
    for (int i = 0; i < 80 * 25; i++) {
        video_mem[i] = (0x0F << 8) | ' ';
    }
    
    vga_text_puts("VIBE OS Booting...\nInitializing video...\n");
    
    /* initialize the screen (vesa or vga) */
    video_init();

    vga_text_puts("Video OK\n");

    /* setup interrupt subsystem */
    kernel_idt_init();
    kernel_pic_init();

    vga_text_puts("Interrupts OK\n");

    /* continue using legacy timer/keyboard for now */
    timer_init(100u);
    keyboard_init();

    /* mouse follows keyboard in existing code */
    mouse_init();

    kernel_irq_enable();  /* unmask lines via new pic code */

    vga_text_puts("IRQ OK\n");

    /* initialize memory subsystem before anything that might need allocation */
    memory_subsystem_init();

    vga_text_puts("Memory OK\n");

    /* setup new subsystems */
    scheduler_init();
    driver_manager_init();

    vga_text_puts("Starting userland...\n");

    /* hand off to userland blob */
    userland_run();

    for (;;) {
        __asm__ volatile("hlt");
    }
}
